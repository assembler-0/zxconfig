/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

import express from "express";
import fs from "fs";
import path from "path";
import os from "os";
import { exec } from "child_process";
import { fileURLToPath } from "url";

export interface ServerOptions {
  /** Directory containing ZXConfig and .zxdsl files */
  projectRoot?: string;
  /** Directory with the built frontend (production only) */
  assetsPath?: string;
  port?: number;
  host?: string;
  open?: boolean;
}

const defaultSchema = `# ZXConfig default template

feature Option1 {
    type: bool;
    label: "Test option 1";
    description: "";
    default: true;
}
`;

function resolvePackageRoot(): string {
  const serverDir = path.dirname(fileURLToPath(import.meta.url));
  return path.join(serverDir, "..");
}

function defaultAssetsPath(): string {
  return path.join(resolvePackageRoot(), "dist", "client");
}

function openBrowser(url: string): void {
  const command =
    process.platform === "darwin"
      ? "open"
      : process.platform === "win32"
        ? "start"
        : "xdg-open";
  exec(`${command} ${JSON.stringify(url)}`);
}

export async function startServer(options: ServerOptions = {}) {
  const app = express();
  const projectRoot = path.resolve(options.projectRoot ?? process.cwd());
  const port = options.port ?? 3000;
  const host = options.host ?? "0.0.0.0";
  const isProduction = process.env.NODE_ENV === "production";

  app.use(express.json());

  const zxConfigPath = path.join(projectRoot, "ZXConfig");
  const zxValuesPath = path.join(projectRoot, "ZXConfig.values");

  // API: Get live core system dump
  app.get("/api/sysdump", (req, res) => {
    try {
      const cpus = os.cpus();
      const sysDump = {
        success: true,
        nodeVersion: process.version,
        arch: os.arch(),
        platform: os.platform(),
        type: os.type(),
        release: os.release(),
        totalMem: os.totalmem(),
        freeMem: os.freemem(),
        cpuModel: cpus.length > 0 ? cpus[0].model : "Unknown CPU",
        cpuThreads: cpus.length,
        uptime: os.uptime(),
        cwd: projectRoot,
        pid: process.pid,
        env: process.env.NODE_ENV || "development",
      };
      return res.json(sysDump);
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  // API: Get active configuration file statuses
  app.get("/api/status", (req, res) => {
    try {
      const fileExists = fs.existsSync(zxConfigPath);
      if (!fileExists) {
        return res.json({
          fileExists: false,
          error:
            "Unrecoverable error: 'ZXConfig' file is missing in project root.",
        });
      }

      const schemaContent = fs.readFileSync(zxConfigPath, "utf-8");

      let valuesContent = {};
      if (fs.existsSync(zxValuesPath)) {
        try {
          valuesContent = JSON.parse(fs.readFileSync(zxValuesPath, "utf-8"));
        } catch (e) {
          valuesContent = {};
        }
      }

      return res.json({
        fileExists: true,
        schema: schemaContent,
        values: valuesContent,
      });
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  // API: Get all DSL and config files
  app.get("/api/files", (req, res) => {
    try {
      const files: Record<string, string> = {};
      if (fs.existsSync(zxConfigPath)) {
        files["ZXConfig"] = fs.readFileSync(zxConfigPath, "utf-8");
      } else {
        files["ZXConfig"] = defaultSchema;
        fs.writeFileSync(zxConfigPath, defaultSchema, "utf-8");
      }

      const items = fs.readdirSync(projectRoot);
      for (const item of items) {
        if (item.endsWith(".zxdsl")) {
          files[item] = fs.readFileSync(
            path.join(projectRoot, item),
            "utf-8",
          );
        }
      }

      return res.json({ success: true, files });
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  // API: Save an individual file
  app.post("/api/save-file", (req, res) => {
    try {
      const { filename, content } = req.body;
      if (!filename || content === undefined) {
        return res
          .status(400)
          .json({ error: "Missing filename or content parameter." });
      }

      const sanitized = filename.replace(/[^a-zA-Z0-9_\-\.]/g, "");
      if (sanitized !== filename) {
        return res
          .status(400)
          .json({ error: "Illegal characters detected in filename." });
      }

      const targetPath = path.join(projectRoot, filename);
      fs.writeFileSync(targetPath, content, "utf-8");
      return res.json({ success: true });
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  // API: Delete an individual file
  app.post("/api/delete-file-spec", (req, res) => {
    try {
      const { filename } = req.body;
      if (!filename) {
        return res.status(400).json({ error: "Missing filename parameter." });
      }
      if (filename === "ZXConfig") {
        return res
          .status(400)
          .json({ error: "The primary 'ZXConfig' file cannot be deleted." });
      }

      const targetPath = path.join(projectRoot, filename);
      if (fs.existsSync(targetPath)) {
        fs.unlinkSync(targetPath);
      }
      return res.json({ success: true });
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  // API: Sync and write code generators
  app.post("/api/sync-generators", (req, res) => {
    try {
      const { files } = req.body;
      if (!Array.isArray(files)) {
        return res
          .status(400)
          .json({ error: "Missing files array inside payload." });
      }

      const synced: string[] = [];
      for (const file of files) {
        if (!file.path || typeof file.content !== "string") continue;

        const absolutePath = path.resolve(projectRoot, file.path);
        const dirName = path.dirname(absolutePath);
        if (!fs.existsSync(dirName)) {
          fs.mkdirSync(dirName, { recursive: true });
        }

        fs.writeFileSync(absolutePath, file.content, "utf-8");
        synced.push(file.path);
      }

      return res.json({ success: true, synced });
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  // API: Save changes to configurations
  app.post("/api/save", (req, res) => {
    try {
      const { schema, values } = req.body;

      if (schema !== undefined) {
        fs.writeFileSync(zxConfigPath, schema, "utf-8");
      }

      if (values !== undefined) {
        fs.writeFileSync(
          zxValuesPath,
          JSON.stringify(values, null, 2),
          "utf-8",
        );
      }

      return res.json({ success: true });
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  // API: Reset or initialize files
  app.post("/api/initialize", (req, res) => {
    try {
      fs.writeFileSync(zxConfigPath, defaultSchema, "utf-8");
      if (fs.existsSync(zxValuesPath)) {
        fs.unlinkSync(zxValuesPath);
      }
      return res.json({ success: true, schema: defaultSchema });
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  // API: Delete ZXConfig strictly to test system error booting state requested!
  app.delete("/api/delete-file", (req, res) => {
    try {
      if (fs.existsSync(zxConfigPath)) {
        fs.unlinkSync(zxConfigPath);
      }
      if (fs.existsSync(zxValuesPath)) {
        fs.unlinkSync(zxValuesPath);
      }
      return res.json({ success: true });
    } catch (err: any) {
      return res.status(500).json({ error: err.message });
    }
  });

  if (!isProduction) {
    const { createServer: createViteServer } = await import("vite");
    const vite = await createViteServer({
      server: { middlewareMode: true },
      appType: "spa",
    });
    app.use(vite.middlewares);
  } else {
    const assetsPath = path.resolve(options.assetsPath ?? defaultAssetsPath());
    if (!fs.existsSync(assetsPath)) {
      throw new Error(
        `Frontend assets not found at ${assetsPath}. Run "npm run build" first.`,
      );
    }

    app.use(express.static(assetsPath));
    app.get("*", (req, res) => {
      res.sendFile(path.join(assetsPath, "index.html"));
    });
  }

  const url = `http://${host === "0.0.0.0" ? "localhost" : host}:${port}`;

  app.listen(port, host, () => {
    console.log(`ZXConfig server running on ${url}`);
    console.log(`Project directory: ${projectRoot}`);
    if (options.open) {
      openBrowser(url);
    }
  });
}

const serverPath = fileURLToPath(import.meta.url);
const invokedPath = process.argv[1] ? path.resolve(process.argv[1]) : "";

if (serverPath === invokedPath || invokedPath.endsWith("server.ts")) {
  startServer().catch((err) => {
    console.error(err);
    process.exit(1);
  });
}
