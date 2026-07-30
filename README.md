# Japanese IR Email Alert

This repository contains a C11/libcurl checker for two Japanese company IR feeds:

- TANAKEN: `https://www.tanaken-1982.co.jp/ja/ir.html`
- Inuneko Seikatsu: `https://corp.inuneko-seikatsu.co.jp/ir/`

The Inuneko page renders its list from the XJ Storage JSONP feed used by the site. The checker reads that feed directly, so it does not depend on JavaScript execution or browser automation.

Each document URL is stored with a company prefix in `state/seen.txt`. The first normal run records the current backlog without sending email. Later runs send one email containing all unseen announcements from either company. State is written only after successful email delivery.

## Local Build

Requirements:

- A C11 compiler
- libcurl development headers and library

On macOS with Homebrew:

```sh
brew install curl
make CPPFLAGS="-I$(brew --prefix curl)/include" LDLIBS="-L$(brew --prefix curl)/lib -lcurl"
```

On Ubuntu/Debian:

```sh
sudo apt-get install build-essential libcurl4-openssl-dev
make
```

Run the offline parser and state tests:

```sh
make test
```

For a live dry run:

```sh
./tanaken-alert --dry-run
```

Use `./tanaken-alert --initialize` to explicitly record the current entries without sending email. Use `./tanaken-alert --test-latest --dry-run` to display the newest entry across both companies. `--test-latest` sends a one-off email without changing state; set `TEST_SUBJECT` if a custom test subject is needed.

## Configuration

Copy `.env.example` to `.env` and keep `.env` out of version control. It is shell syntax, so quote values containing spaces:

```sh
cp .env.example .env
set -a
. ./.env
set +a
./tanaken-alert --dry-run
```

Source variables:

- `TANAKEN_URL`: optional TANAKEN page override
- `INUNEKO_URL`: optional Inuneko IR page override
- `INUNEKO_API_URL`: optional XJ Storage feed override, mainly for tests
- `STATE_FILE`: state path, default `state/seen.txt`

SMTP variables:

- `SMTP_URL`: for example `smtps://smtp.gmail.com:465` or `smtp://smtp.example.com:587`
- `SMTP_USERNAME`: SMTP login, optional only for an unauthenticated relay
- `SMTP_PASSWORD`: SMTP password or provider app password
- `ALERT_FROM`: envelope sender and `From` header
- `ALERT_TO`: one address, or comma/semicolon-separated addresses
- `ALERT_SUBJECT`: optional normal-alert subject override; leave unset for a company-specific subject
- `TEST_SUBJECT`: optional `--test-latest` subject override; leave unset for a company-specific subject

For Gmail, use an app password rather than the regular account password. Never commit SMTP credentials.

## GitHub Deployment With `gh`

Authenticate and create a private repository from this directory:

```sh
gh auth login
git init
git add .
git commit -m "Initial Japanese IR alert"
gh repo create japanese-ir-alert --private --source=. --remote=origin --push
```

Add the SMTP values as repository secrets. Running each command without `--body` lets `gh` read the value without placing it in shell history:

```sh
gh secret set SMTP_URL
gh secret set SMTP_USERNAME
gh secret set SMTP_PASSWORD
gh secret set ALERT_FROM
gh secret set ALERT_TO
```

Optionally set custom subjects as repository variables. Leave these variables unset to have the program identify the company automatically:

```sh
gh variable set ALERT_SUBJECT --body "New IR announcement"
gh variable set TEST_SUBJECT --body "Latest IR announcement"
```

Start and verify the workflow:

```sh
gh workflow run ir-alert.yml --ref main
gh run list --workflow ir-alert.yml --limit 1 --json databaseId,status,conclusion
gh run watch RUN_ID --exit-status
```

The first successful run initializes the shared state and sends no backlog email. The scheduled workflow checks every 15 minutes. GitHub may delay scheduled workflows on busy runners.

The workflow separates the read-only check/send job from the write-only state job. The state job pushes only to the repository default branch and requires `contents: write`.
