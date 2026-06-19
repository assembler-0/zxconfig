# ZXconfig

Web-based configuration engine for ZXConfig files — a Kconfig-like workflow with a browser UI.

Run it in any project that has a `ZXConfig` file in the root. The UI reads and writes config there; generated outputs and `.zxdsl` files stay in your project directory.

## Requirements

- Node.js 20 or newer

## Install

**One-off (no install):**

```bash
npx @assembler-0/zxconfig
```

**Global CLI:**

```bash
npm install -g @assembler-0/zxconfig
```

**Project dev dependency:**

```bash
npm install --save-dev @assembler-0/zxconfig
```

Then add a script to `package.json`:

```json
{
  "scripts": {
    "config": "zxconfig"
  }
}
```

## Usage

From your project root (where `ZXConfig` lives):

```bash
cd my-project
zxconfig
```

Open [http://localhost:3000](http://localhost:3000) in your browser if it does not open automatically.

### Options

```
zxconfig [options]

  -C, --dir <path>    Project directory (default: current directory)
  -p, --port <port>   Port to listen on (default: 3000)
  -H, --host <host>   Host to bind (default: 0.0.0.0)
  -o, --open          Open browser after startup
  -h, --help          Show help
  -v, --version       Show version
```

Examples:

```bash
zxconfig --open
zxconfig --port 8080 --dir ./firmware
npx @assembler-0/zxconfig -C . -o
```

## Project files

ZXconfig works on files in your **project directory**, not inside the npm package:

| File | Purpose |
|------|---------|
| `ZXConfig` | Main configuration schema |
| `ZXConfig.values` | Saved option values (JSON, created on save) |
| `*.zxdsl` | DSL / generator definitions |

If `ZXConfig` is missing, the app can create a default template on first use.

## Development

Clone and run from source:

```bash
git clone https://github.com/assembler-0/ZXconfig
cd ZXconfig
npm install
npm run dev
```

Build and run the production CLI locally:

```bash
npm run build
npm start
```

Other scripts:

```bash
npm run lint    # TypeScript check
npm run clean   # Remove dist/
```

## Publishing

Maintainers: bump `version` in `package.json`, then either:

```bash
npm publish
```

or create a GitHub Release (tag `v*`) to trigger the publish workflow (requires `NPM_TOKEN` in repo secrets). The package is published as `@assembler-0/zxconfig` (public scoped package).

## License

Apache-2.0
