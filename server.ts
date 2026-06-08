/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

import express from "express";
import fs from "fs";
import path from "path";
import os from "os";
import { createServer as createViteServer } from "vite";

async function startServer() {
  const app = express();
  const PORT = 3000;

  app.use(express.json());

  const zxConfigPath = path.join(process.cwd(), "ZXConfig");
  const zxValuesPath = path.join(process.cwd(), "ZXConfig.values");

  const defaultSchema = `# ZXConfig default template

feature Option1 {
    type: bool;
    label: "Test option 1";
    description: "";
    default: true;
}
`;

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
        cwd: process.cwd(),
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
      // Ensure ZXConfig exists
      if (fs.existsSync(zxConfigPath)) {
        files["ZXConfig"] = fs.readFileSync(zxConfigPath, "utf-8");
      } else {
        files["ZXConfig"] = defaultSchema;
        fs.writeFileSync(zxConfigPath, defaultSchema, "utf-8");
      }

      const items = fs.readdirSync(process.cwd());
      for (const item of items) {
        if (item.endsWith(".zxdsl")) {
          files[item] = fs.readFileSync(
            path.join(process.cwd(), item),
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

      const targetPath = path.join(process.cwd(), filename);
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

      const targetPath = path.join(process.cwd(), filename);
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

        const absolutePath = path.resolve(process.cwd(), file.path);
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

  // Vite integration middleware
  if (process.env.NODE_ENV !== "production") {
    const vite = await createViteServer({
      server: { middlewareMode: true },
      appType: "spa",
    });
    app.use(vite.middlewares);
  } else {
    const distPath = path.join(process.cwd(), "dist");
    app.use(express.static(distPath));
    app.get("*", (req, res) => {
      res.sendFile(path.join(distPath, "index.html"));
    });
  }

  app.listen(PORT, "0.0.0.0", () => {
    console.log(`ZXConfig server running on http://0.0.0.0:${PORT}`);
  });
}

startServer();
