# Voice STT — AWS Transcribe Streaming Lambda

The board firmware POSTs raw 16 kHz mono 16-bit PCM to this Lambda; the
Lambda streams it to AWS Transcribe and returns `{"text": "..."}`. The
firmware then types that text into the focused chat input via BLE HID
`Unicode Hex Input` keystrokes (see `src/ble_hid.cpp::bleHidTypeUtf8`).

```
ESP32-S3 ──HTTPS POST (PCM)──▶  Lambda Function URL  ──▶  Transcribe Streaming
                ◀─────── JSON {"text":"...","ms":N} ───────
```

## Prerequisites

- AWS account with the `buddy` profile in `~/.aws/credentials`
  (override with `AWS_PROFILE=...` if your profile is named differently)
- `awscli` v2, `python3` (3.9+) with `pip`, `openssl`, `zip`
- Region defaults to `ap-northeast-2` (Seoul). Override with
  `AWS_REGION=us-east-1 ./deploy.sh` etc.

## One-shot deploy

```bash
cd tools/lambda_transcribe
./deploy.sh
```

The script is idempotent — re-running it updates the Lambda in place
and reuses the API key in `.api_key` so the firmware doesn't need
re-flashing on redeploy.

On first run it prints two values to paste into `src/secrets.h`:

```c
#define STT_ENDPOINT_URL  "https://xxx.lambda-url.ap-northeast-2.on.aws"
#define STT_API_KEY       "<48 hex chars>"
```

Then reflash the firmware:

```bash
pio run -e waveshare-amoled -t upload
```

## What gets created in AWS

| Resource | Name | Notes |
|----------|------|-------|
| IAM role | `claude-buddy-transcribe-role` | Trust: `lambda.amazonaws.com`. Inline policy with `transcribe:StartStreamTranscription` + `AWSLambdaBasicExecutionRole` for CloudWatch. |
| Lambda function | `claude-buddy-transcribe` | Python 3.11, 512 MB, 30 s timeout. Env: `LANGUAGE_CODE=ko-KR`, `SAMPLE_RATE=16000`, `API_KEY=<hex>`. |
| Function URL | (auto-generated) | `auth-type NONE` — auth is enforced in code by comparing the `x-api-key` header to the `API_KEY` env var. |

Names are configurable via env vars (`FUNCTION_NAME`, `ROLE_NAME`).

## Switching language

`LANGUAGE_CODE` is an env var on the Lambda — to switch from Korean to
English without redeploying:

```bash
aws --profile buddy --region ap-northeast-2 lambda update-function-configuration \
  --function-name claude-buddy-transcribe \
  --environment 'Variables={LANGUAGE_CODE=en-US,SAMPLE_RATE=16000,API_KEY=<paste from .api_key>}'
```

(Or just re-run `LANGUAGE_CODE=en-US ./deploy.sh`.)

Supported codes: any value Transcribe Streaming accepts —
`ko-KR`, `en-US`, `en-GB`, `ja-JP`, `zh-CN`, etc. See the
[Transcribe docs](https://docs.aws.amazon.com/transcribe/latest/dg/supported-languages.html).

## Testing without the firmware

Record a few seconds of 16 kHz mono 16-bit PCM with `sox` or `arecord`,
then POST it directly:

```bash
# macOS:
sox -d -r 16000 -c 1 -b 16 -e signed-integer test.pcm trim 0 5

curl -X POST "$(<.url 2>/dev/null || cat)" \
  -H "x-api-key: $(cat .api_key)" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @test.pcm
```

(`.url` is not auto-saved — copy from the deploy output. Or just use
`aws lambda get-function-url-config` to fetch it.)

## CloudWatch logs

```bash
aws --profile buddy --region ap-northeast-2 logs tail \
  /aws/lambda/claude-buddy-transcribe --follow
```

Each invocation logs the round-trip in `ms` and any Transcribe errors.

## Cost (personal use, ~5 min/day)

| Service | Rate | Monthly |
|---------|------|---------|
| Transcribe Streaming | $0.024/min | ~$3.60 |
| Lambda compute (512 MB × ~10 s × 30/day) | ~$0.001/invoke | ~$0.03 |
| Function URL | free | $0 |
| CloudWatch logs | free tier covers it | ~$0 |

Total: ~$4/month at typical hobby usage.

## Teardown

```bash
aws --profile buddy --region ap-northeast-2 lambda delete-function-url-config \
  --function-name claude-buddy-transcribe
aws --profile buddy --region ap-northeast-2 lambda delete-function \
  --function-name claude-buddy-transcribe
aws --profile buddy iam delete-role-policy \
  --role-name claude-buddy-transcribe-role --policy-name transcribe-streaming
aws --profile buddy iam detach-role-policy \
  --role-name claude-buddy-transcribe-role \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
aws --profile buddy iam delete-role --role-name claude-buddy-transcribe-role
rm -f .api_key lambda.zip && rm -rf build/
```

## Troubleshooting

**`pip install --platform manylinux2014_x86_64` fails**: needs pip 22.0+.
Upgrade with `python3 -m pip install --upgrade pip`.

**`No matching distribution found for awscrt`**: your local Python
version differs from `--python-version 3.11`. The flag forces the
metadata filter but pip still uses your interpreter — install a Python
3.11 via `pyenv install 3.11` or `brew install python@3.11`, then
re-run the deploy.

**Lambda returns `{"error":"transcribe failed", ...}`**: check
CloudWatch logs (above). Common causes: IAM policy missing (re-run
deploy to recreate), Transcribe service quota, audio format mismatch
(must be 16 kHz mono 16-bit signed PCM).

**Lambda returns `{"error":"audio too short"}`**: the firmware sent
under 0.5 s of audio. Hold Key1 longer, or check serial logs for
`[stt] mic stop, n=NNN samples` — should be ≥ 8000 samples.

**401 invalid api key**: the API key in `src/secrets.h` doesn't match
the `API_KEY` env var on the Lambda. Either redeploy
(reuses `.api_key`) or copy `cat .api_key` into `secrets.h`.
