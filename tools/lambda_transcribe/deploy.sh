#!/usr/bin/env bash
# Idempotent deploy of the Transcribe Lambda + Function URL.
#
# Re-running updates the existing function in place. Generates a random
# API key on first run (stored in .api_key, gitignored) and reuses it
# afterwards so the firmware doesn't need re-flashing on redeploy.
#
# Requires: bash, awscli, openssl, python3 (with pip), zip.
set -euo pipefail

# ─── Config ─────────────────────────────────────────────────────────────
PROFILE="${AWS_PROFILE:-buddy}"
REGION="${AWS_REGION:-ap-northeast-2}"
FUNCTION_NAME="${FUNCTION_NAME:-claude-buddy-transcribe}"
ROLE_NAME="${ROLE_NAME:-${FUNCTION_NAME}-role}"
RUNTIME="python3.11"
TIMEOUT=30
MEMORY_MB=512
LANGUAGE_CODE="${LANGUAGE_CODE:-ko-KR}"
SAMPLE_RATE="${SAMPLE_RATE:-16000}"

# ─── Setup ──────────────────────────────────────────────────────────────
cd "$(dirname "$0")"

aws_cmd() { aws --profile "$PROFILE" --region "$REGION" "$@"; }

echo "==> Verifying AWS profile '$PROFILE'"
ACCOUNT_ID=$(aws_cmd sts get-caller-identity --query 'Account' --output text)
echo "    account=$ACCOUNT_ID region=$REGION"

# ─── API key (generate once, reuse) ─────────────────────────────────────
if [[ -f .api_key ]]; then
  API_KEY=$(cat .api_key)
  echo "==> Reusing existing API key from .api_key"
else
  API_KEY=$(openssl rand -hex 24)
  printf '%s' "$API_KEY" > .api_key
  chmod 600 .api_key
  echo "==> Generated new API key (saved to .api_key)"
fi

# ─── Build deployment zip ───────────────────────────────────────────────
echo "==> Packaging Lambda zip"
rm -rf build lambda.zip
mkdir build

# Force Linux x86_64 wheels for Python 3.11 — Lambda runs in that env,
# and amazon-transcribe pulls awscrt which has native code.
python3 -m pip install --quiet --target build/ \
  --platform manylinux2014_x86_64 \
  --python-version 3.11 \
  --implementation cp \
  --only-binary=:all: \
  amazon-transcribe

cp lambda_function.py build/
( cd build && zip -qr ../lambda.zip . )
ZIP_SIZE=$(du -h lambda.zip | cut -f1)
echo "    lambda.zip: $ZIP_SIZE"

# ─── IAM role ───────────────────────────────────────────────────────────
echo "==> Ensuring IAM role $ROLE_NAME"
TRUST='{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"lambda.amazonaws.com"},"Action":"sts:AssumeRole"}]}'

if ROLE_ARN=$(aws_cmd iam get-role --role-name "$ROLE_NAME" --query 'Role.Arn' --output text 2>/dev/null); then
  echo "    role exists: $ROLE_ARN"
else
  ROLE_ARN=$(aws_cmd iam create-role \
    --role-name "$ROLE_NAME" \
    --assume-role-policy-document "$TRUST" \
    --query 'Role.Arn' --output text)
  echo "    role created: $ROLE_ARN"

  aws_cmd iam attach-role-policy \
    --role-name "$ROLE_NAME" \
    --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole

  aws_cmd iam put-role-policy \
    --role-name "$ROLE_NAME" \
    --policy-name "transcribe-streaming" \
    --policy-document '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"transcribe:StartStreamTranscription","Resource":"*"}]}'

  echo "    waiting 10s for IAM eventual consistency..."
  sleep 10
fi

# ─── Lambda function (create or update) ─────────────────────────────────
ENV_VARS="Variables={LANGUAGE_CODE=$LANGUAGE_CODE,SAMPLE_RATE=$SAMPLE_RATE,API_KEY=$API_KEY}"

if aws_cmd lambda get-function --function-name "$FUNCTION_NAME" >/dev/null 2>&1; then
  echo "==> Updating Lambda code + config"
  aws_cmd lambda update-function-code \
    --function-name "$FUNCTION_NAME" \
    --zip-file "fileb://lambda.zip" >/dev/null
  aws_cmd lambda wait function-updated --function-name "$FUNCTION_NAME"
  aws_cmd lambda update-function-configuration \
    --function-name "$FUNCTION_NAME" \
    --runtime "$RUNTIME" \
    --timeout "$TIMEOUT" \
    --memory-size "$MEMORY_MB" \
    --environment "$ENV_VARS" >/dev/null
  aws_cmd lambda wait function-updated --function-name "$FUNCTION_NAME"
else
  echo "==> Creating Lambda"
  aws_cmd lambda create-function \
    --function-name "$FUNCTION_NAME" \
    --runtime "$RUNTIME" \
    --role "$ROLE_ARN" \
    --handler lambda_function.lambda_handler \
    --timeout "$TIMEOUT" \
    --memory-size "$MEMORY_MB" \
    --environment "$ENV_VARS" \
    --zip-file "fileb://lambda.zip" >/dev/null
  aws_cmd lambda wait function-active --function-name "$FUNCTION_NAME"
fi

# ─── Function URL (create once, reuse) ──────────────────────────────────
if URL=$(aws_cmd lambda get-function-url-config \
  --function-name "$FUNCTION_NAME" --query 'FunctionUrl' --output text 2>/dev/null); then
  echo "==> Function URL exists: $URL"
else
  URL=$(aws_cmd lambda create-function-url-config \
    --function-name "$FUNCTION_NAME" \
    --auth-type NONE \
    --query 'FunctionUrl' --output text)
  echo "==> Function URL created: $URL"

  aws_cmd lambda add-permission \
    --function-name "$FUNCTION_NAME" \
    --statement-id "FunctionURLAllowPublicInvokeFunctionUrl" \
    --action lambda:InvokeFunctionUrl \
    --principal "*" \
    --function-url-auth-type NONE >/dev/null

  # Some account/org configurations also require lambda:InvokeFunction
  # alongside InvokeFunctionUrl for unauthenticated Function URL access
  # (the AWS Console flags this with a missing-permission warning).
  # The function-url-auth-type condition is rejected for InvokeFunction
  # — that condition only applies to InvokeFunctionUrl.
  aws_cmd lambda add-permission \
    --function-name "$FUNCTION_NAME" \
    --statement-id "FunctionURLAllowPublicInvokeFunction" \
    --action lambda:InvokeFunction \
    --principal "*" >/dev/null
fi

URL_TRIMMED="${URL%/}"

# ─── Output ─────────────────────────────────────────────────────────────
cat <<EOF

╔══════════════════════════════════════════════════════════════════════╗
║  Deploy complete                                                     ║
╚══════════════════════════════════════════════════════════════════════╝

Paste these into src/secrets.h, then re-flash the firmware:

  #define STT_ENDPOINT_URL  "$URL_TRIMMED"
  #define STT_API_KEY       "$API_KEY"

Smoke test (record 3 s of speech to test.pcm, post it):
  pio device monitor -e waveshare-amoled         # in another terminal
  curl -X POST "$URL_TRIMMED" \\
    -H "x-api-key: $API_KEY" \\
    -H "Content-Type: application/octet-stream" \\
    --data-binary @test.pcm

CloudWatch logs:
  aws --profile $PROFILE --region $REGION logs tail /aws/lambda/$FUNCTION_NAME --follow

EOF
