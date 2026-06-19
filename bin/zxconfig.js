#!/usr/bin/env node

import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { readFileSync } from "node:fs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.join(__dirname, "..");

function readVersion() {
  try {
    const pkg = JSON.parse(
      readFileSync(path.join(packageRoot, "package.json"), "utf-8"),
    );
    return pkg.version;
  } catch {
    return "unknown";
  }
}

function printHelp() {
  console.log(`ZXConfig — web-based configuration engine

Usage:
  zxconfig [options]

Options:
  -C, --dir <path>    Project directory containing ZXConfig (default: cwd)
  -p, --port <port>   Port to listen on (default: 3000)
  -H, --host <host>   Host to bind (default: 0.0.0.0)
  -o, --open          Open browser after startup
  -h, --help          Show this help message
  -v, --version       Show version number
`);
}

function parseArgs(argv) {
  const options = {
    dir: process.cwd(),
    port: 3000,
    host: "0.0.0.0",
    open: false,
    help: false,
    version: false,
  };

  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];

    switch (arg) {
      case "--dir":
      case "-C":
        options.dir = path.resolve(argv[++i] ?? "");
        break;
      case "--port":
      case "-p":
        options.port = Number(argv[++i]);
        break;
      case "--host":
      case "-H":
        options.host = argv[++i] ?? options.host;
        break;
      case "--open":
      case "-o":
        options.open = true;
        break;
      case "--help":
      case "-h":
        options.help = true;
        break;
      case "--version":
      case "-v":
        options.version = true;
        break;
      default:
        console.error(`Unknown option: ${arg}`);
        printHelp();
        process.exit(1);
    }
  }

  if (Number.isNaN(options.port) || options.port <= 0) {
    console.error("Invalid port number.");
    process.exit(1);
  }

  return options;
}

const cli = parseArgs(process.argv);

if (cli.help) {
  printHelp();
  process.exit(0);
}

if (cli.version) {
  console.log(readVersion());
  process.exit(0);
}

process.env.NODE_ENV = "production";

const serverPath = path.join(packageRoot, "dist", "server.mjs");
const { startServer } = await import(pathToFileURL(serverPath).href);

await startServer({
  projectRoot: cli.dir,
  port: cli.port,
  host: cli.host,
  open: cli.open,
});
