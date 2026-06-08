/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

import React, { useState, useEffect, useRef } from "react";
import {
  X,
  Minimize2,
  Maximize2,
  FileCode,
  Sliders,
  Settings,
  Layers,
  Terminal,
  AlertTriangle,
  RotateCcw,
  Trash2,
  Save,
  FileText,
  Check,
  RefreshCw,
  Clock,
  HelpCircle,
} from "lucide-react";
import {
  parseZXDSL,
  evaluateExpression,
  isNodeVisible,
  evaluateConstraints,
} from "./parser";
import {
  generateCHeader,
  generateRustModule,
  generateMakefile,
  generateDevEnv,
  generateFormat,
} from "./generators";
import {
  ZXDocument,
  ConfigValuesMap,
  WindowInstance,
  ParseError,
} from "./types";

export function highlightZXDSL(line: string): React.ReactNode {
  if (!line) return <span className="min-h-[1.4rem]">&nbsp;</span>;
  const regex =
    /(\".*?\"|#.*|\/\/.*|[a-zA-Z_][a-zA-Z0-9_]*|[0-9]+|==|!=|<=|>=|&&|\|\||[{}[\]:;,()!<>])/g;
  const parts = line.split(regex);
  const keywords = [
    "feature",
    "option",
    "group",
    "type",
    "label",
    "description",
    "default",
    "range",
    "values",
    "visible_when",
    "constraint",
    "include",
    "generate",
  ];
  const types = ["bool", "int", "string", "select"];

  return parts.map((part, i) => {
    if (!part) return null;
    if (part.startsWith("#") || part.startsWith("//")) {
      return (
        <span key={i} className="text-[#8b949e] italic font-mono">
          {part}
        </span>
      );
    }
    if (part.startsWith('"') && part.endsWith('"')) {
      return (
        <span key={i} className="text-[#a5d6ff] font-mono font-semibold">
          {part}
        </span>
      );
    }
    if (keywords.includes(part)) {
      return (
        <span key={i} className="text-[#ff7b72] font-semibold font-mono">
          {part}
        </span>
      );
    }
    if (types.includes(part)) {
      return (
        <span key={i} className="text-[#79c0ff] font-medium font-mono">
          {part}
        </span>
      );
    }
    if (part === "true" || part === "false") {
      return (
        <span key={i} className="text-[#58a6ff] font-semibold font-mono">
          {part}
        </span>
      );
    }
    if (/^[0-9]+$/.test(part)) {
      return (
        <span key={i} className="text-[#79c0ff] font-mono">
          {part}
        </span>
      );
    }
    if (["==", "!=", "<=", ">=", "&&", "||", "!", "<", ">"].includes(part)) {
      return (
        <span key={i} className="text-[#ff7b72] font-semibold font-mono">
          {part}
        </span>
      );
    }
    if (["{", "}", "[", "]", ":", ";", ",", "(", ")"].includes(part)) {
      return (
        <span key={i} className="text-[#8b949e] font-mono">
          {part}
        </span>
      );
    }
    if (/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(part)) {
      return (
        <span key={i} className="text-[#e6edf3] font-mono">
          {part}
        </span>
      );
    }
    return (
      <span key={i} className="font-mono text-[#8b949e]">
        {part}
      </span>
    );
  });
}

const INITIAL_WINDOWS: WindowInstance[] = [
  {
    id: "editor",
    title: "ZXDSL Specification Editor",
    x: 20,
    y: 60,
    width: 620,
    height: 490,
    zIndex: 10,
    isOpen: true,
    isMinimized: false,
    isMaximized: false,
  },
  {
    id: "gui",
    title: "Config Panel",
    x: 660,
    y: 60,
    width: 550,
    height: 600,
    zIndex: 10,
    isOpen: true,
    isMinimized: false,
    isMaximized: false,
  },
  {
    id: "compiler",
    title: "Compiler & Exporter Engine",
    x: 20,
    y: 570,
    width: 620,
    height: 350,
    zIndex: 5,
    isOpen: true,
    isMinimized: false,
    isMaximized: false,
  },
  {
    id: "ast",
    title: "AST & Dependency Diagnostics",
    x: 1230,
    y: 490,
    width: 440,
    height: 430,
    zIndex: 5,
    isOpen: false,
    isMinimized: false,
    isMaximized: false,
  },
  {
    id: "fs",
    title: "Host System & Server Console",
    x: 1230,
    y: 60,
    width: 440,
    height: 410,
    zIndex: 10,
    isOpen: true,
    isMinimized: false,
    isMaximized: false,
  },
  {
    id: "about",
    title: "About & Core System Dump",
    x: 350,
    y: 180,
    width: 540,
    height: 480,
    zIndex: 12,
    isOpen: false,
    isMinimized: false,
    isMaximized: false,
  },
];

export default function App() {
  // Config & state
  const [bootSuccess, setBootSuccess] = useState<boolean | null>(null);
  const [bootError, setBootError] = useState<string>("");
  const [filesMap, setFilesMap] = useState<Record<string, string>>({});
  const [currentFilename, setCurrentFilename] = useState<string>("ZXConfig");
  const [configValues, setConfigValues] = useState<ConfigValuesMap>({});
  const [serverLogs, setServerLogs] = useState<string[]>([]);

  // Kconfig-style startup Cache loading states
  const [cachePromptPending, setCachePromptPending] = useState<boolean>(false);
  const [staleCacheValues, setStaleCacheValues] = useState<ConfigValuesMap>({});

  // AST representations
  const [parsedDoc, setParsedDoc] = useState<ZXDocument>({
    groups: [],
    optionsMap: {},
  });
  const [parseErrors, setParseErrors] = useState<ParseError[]>([]);

  // Custom Window management
  const [windows, setWindows] = useState<WindowInstance[]>(INITIAL_WINDOWS);
  const [activeTab, setActiveTab] = useState<"c" | "rust" | "makefile" | "env">(
    "c",
  );
  const [dependencyTraceId, setDependencyTraceId] = useState<string>("");
  const [copiedId, setCopiedId] = useState<string | null>(null);
  const [isAutoSaving, setIsAutoSaving] = useState<boolean>(true);

  // Dragging states
  const [isDragging, setIsDragging] = useState<boolean>(false);
  const [draggedWinId, setDraggedWinId] = useState<string | null>(null);
  const [dragStart, setDragStart] = useState<{
    x: number;
    y: number;
    winX: number;
    winY: number;
  } | null>(null);
  const maxZIndexRef = useRef<number>(20);

  // Window Resizing states
  const [isResizing, setIsResizing] = useState<boolean>(false);
  const [resizedWinId, setResizedWinId] = useState<string | null>(null);
  const [resizeStart, setResizeStart] = useState<{
    x: number;
    y: number;
    winWidth: number;
    winHeight: number;
  } | null>(null);

  // System dump states
  const [sysDumpData, setSysDumpData] = useState<any>(null);
  const [sysDumpLoading, setSysDumpLoading] = useState<boolean>(false);

  const fetchSysDump = async () => {
    try {
      setSysDumpLoading(true);
      const res = await fetch("/api/sysdump");
      if (res.ok) {
        const data = await res.json();
        setSysDumpData(data);
      }
    } catch (err) {
      console.error("System Dump fetch failure:", err);
    } finally {
      setSysDumpLoading(false);
    }
  };

  const handleResizeMouseDown = (id: string, e: React.MouseEvent) => {
    const win = windows.find((w) => w.id === id);
    if (!win || win.isMaximized) return;

    focusWindow(id);
    setIsResizing(true);
    setResizedWinId(id);
    setResizeStart({
      x: e.clientX,
      y: e.clientY,
      winWidth: win.width,
      winHeight: win.height,
    });
    e.preventDefault();
    e.stopPropagation();
  };

  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const preRef = useRef<HTMLPreElement>(null);
  const gutterRef = useRef<HTMLDivElement>(null);

  const handleScroll = () => {
    if (textareaRef.current) {
      const { scrollTop, scrollLeft } = textareaRef.current;
      if (preRef.current) {
        preRef.current.scrollTop = scrollTop;
        preRef.current.scrollLeft = scrollLeft;
      }
      if (gutterRef.current) {
        gutterRef.current.scrollTop = scrollTop;
      }
    }
  };

  useEffect(() => {
    if (textareaRef.current) {
      textareaRef.current.scrollTop = 0;
      textareaRef.current.scrollLeft = 0;
    }
    if (preRef.current) {
      preRef.current.scrollTop = 0;
      preRef.current.scrollLeft = 0;
    }
    if (gutterRef.current) {
      gutterRef.current.scrollTop = 0;
    }
  }, [currentFilename]);

  // UTC Clock Tracker
  const [currentTime, setCurrentTime] = useState<string>("");

  const addLog = (msg: string) => {
    const time = new Date().toISOString().split("T")[1].slice(0, 8);
    setServerLogs((prev) => [`[${time}] ${msg}`, ...prev].slice(0, 50));
  };

  // Check boot status (file ZXConfig checker) and load all workspace specifications
  const checkBootStatus = async () => {
    try {
      addLog("Querying server status GET /api/status...");
      const res = await fetch("/api/status");
      const data = await res.json();

      if (res.ok && data.fileExists) {
        setBootSuccess(true);
        setBootError("");
        setConfigValues(data.values || {});
        addLog("Host link established.");

        await loadFiles(data.values || {});
      } else {
        setBootSuccess(false);
        setBootError(
          data.error ||
            "Unrecoverable error: File 'ZXConfig' not detected in project root workspace.",
        );
        addLog(
          "Server connection responded with non-zero check count error flag.",
        );
      }
    } catch (err: any) {
      setBootSuccess(false);
      setBootError(
        `Timeout: Cannot bind to Node process. Error: ${err.message}`,
      );
      addLog(`GET /api/status failed to resolve: ${err.message}`);
    }
  };

  const loadFiles = async (loadedValues?: ConfigValuesMap) => {
    try {
      addLog("Querying workspace list GET /api/files...");
      const res = await fetch("/api/files");
      const data = await res.json();
      if (res.ok && data.success) {
        const files = data.files || {};
        setFilesMap(files);
        addLog(
          `Successfully loaded ${Object.keys(files).length} files from repository.`,
        );

        // Pick active compiling file defaults
        const mainContent = files["ZXConfig"] || "";
        const parsed = parseZXDSL(mainContent, files);
        setParsedDoc(parsed.doc);
        setParseErrors(parsed.errors);

        // Pick an option to trace by default
        const optIds = Object.keys(parsed.doc.optionsMap);
        if (optIds.length > 0 && !dependencyTraceId) {
          setDependencyTraceId(optIds[0]);
        }

        // Kconfig-style cache checker:
        // Cache exists if there is at least one non-empty value key inside loadedValues or configValues
        const activeValues = loadedValues || configValues;
        const cacheKeys = Object.keys(activeValues);
        const optionsKeys = Object.keys(parsed.doc.optionsMap);

        const cacheExists = cacheKeys.length > 0;
        const isMissingOrPartial =
          !cacheExists ||
          optionsKeys.some((key) => activeValues[key] === undefined);

        if (isMissingOrPartial && optionsKeys.length > 0) {
          setCachePromptPending(true);
          setStaleCacheValues({ ...activeValues });
          addLog(
            "Stale or partially uncommitted config cache detected. ZXCOnfig load routine halted.",
          );
        } else {
          setCachePromptPending(false);
        }
      }
    } catch (err: any) {
      addLog(`Failed to compile filesystem files: ${err.message}`);
    }
  };

  // Kconfig choice actions
  const handleLoadFreshDefaults = () => {
    const freshValues: ConfigValuesMap = {};
    Object.keys(parsedDoc.optionsMap).forEach((key) => {
      const opt = parsedDoc.optionsMap[key];
      freshValues[key] =
        opt.defaultValue !== undefined
          ? opt.defaultValue
          : opt.type === "bool"
            ? false
            : opt.type === "int"
              ? 0
              : "";
    });
    setConfigValues(freshValues);
    triggerSaveValues(freshValues);
    setCachePromptPending(false);
    addLog(
      "Loaded ZXConfig-style standard fresh defaults across all spec option nodes.",
    );
  };

  const handleRestoreStaleCache = () => {
    const restoredValues = { ...staleCacheValues };
    Object.keys(parsedDoc.optionsMap).forEach((key) => {
      if (restoredValues[key] === undefined) {
        const opt = parsedDoc.optionsMap[key];
        restoredValues[key] =
          opt.defaultValue !== undefined
            ? opt.defaultValue
            : opt.type === "bool"
              ? false
              : opt.type === "int"
                ? 0
                : "";
      }
    });
    setConfigValues(restoredValues);
    triggerSaveValues(restoredValues);
    setCachePromptPending(false);
    addLog(
      "Restored partial configuration files from partial cache with default fallbacks padded.",
    );
  };

  // Trigger boot system on load
  useEffect(() => {
    checkBootStatus();

    // Set time
    const updateClock = () => {
      const now = new Date();
      const str = now.toISOString().replace("T", " ").slice(0, 19) + " UTC";
      setCurrentTime(str);
    };
    updateClock();
    const interval = setInterval(updateClock, 1000);
    return () => clearInterval(interval);
  }, []);

  // Sync file changes & AST evaluations
  const handleFileChange = (filename: string, newContent: string) => {
    const nextFiles = { ...filesMap, [filename]: newContent };
    setFilesMap(nextFiles);

    // Compile entire schema from root entry ZXConfig
    const mainContent = nextFiles["ZXConfig"] || "";
    const parsed = parseZXDSL(mainContent, nextFiles);
    setParsedDoc(parsed.doc);
    setParseErrors(parsed.errors);

    // Save changes when auto-saving enabled
    if (isAutoSaving) {
      triggerSaveFile(filename, newContent);
    }
  };

  const triggerSaveFile = async (fn: string, contentToSave: string) => {
    try {
      const res = await fetch("/api/save-file", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ filename: fn, content: contentToSave }),
      });
      if (res.ok) {
        addLog(`Saved individual file change: [${fn}] updated successfully.`);
      }
    } catch (err: any) {
      addLog(`FS Save Error on [${fn}]: ${err.message}`);
    }
  };

  // Sync config values
  const handleConfigValueChange = (
    optionId: string,
    val: string | number | boolean,
  ) => {
    const nextValues = { ...configValues, [optionId]: val };
    setConfigValues(nextValues);

    if (isAutoSaving) {
      triggerSaveValues(nextValues);
    }
  };

  const triggerSaveValues = async (valuesToSave: ConfigValuesMap) => {
    try {
      const res = await fetch("/api/save", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ values: valuesToSave }),
      });
      if (res.ok) {
        addLog("Config variables values synchronized successfully.");
      }
    } catch (err: any) {
      addLog(`Values Sync Error: ${err.message}`);
    }
  };

  // Add individual file
  const handleAddNewFile = async (name: string) => {
    const defaultTemplateSpec = `# ZXDSL specification file: ${name}\n\n`;
    try {
      const res = await fetch("/api/save-file", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ filename: name, content: defaultTemplateSpec }),
      });
      if (res.ok) {
        addLog(`File '${name}' created on host storage.`);
        await loadFiles();
        setCurrentFilename(name);
      }
    } catch (err: any) {
      addLog(`Failed to write new file: ${err.message}`);
    }
  };

  // Delete individual file
  const handleDeleteFileSpec = async (name: string) => {
    try {
      const res = await fetch("/api/delete-file-spec", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ filename: name }),
      });
      if (res.ok) {
        addLog(`File '${name}' deleted.`);
        await loadFiles();
        if (currentFilename === name) {
          setCurrentFilename("ZXConfig");
        }
      }
    } catch (err: any) {
      addLog(`Failed to delete file: ${err.message}`);
    }
  };

  // Sync compiler output path sync-generators action
  const handleSyncTarget = async () => {
    const directive = parsedDoc.generates?.find((g) => g.format === activeTab);

    if (!directive) {
      addLog(
        `Sync failure: No explicit 'generate' target found for format '${activeTab}' in active specs.`,
      );
      alert(
        `Sync failure: No 'generate' target defined for format [${activeTab}] inside ZXDSL files.\n\nPlease define e.g.: generate ${activeTab} "path/to/target";`,
      );
      return;
    }

    addLog(`Filing sync to host system at: ${directive.path}`);
    try {
      const res = await fetch("/api/sync-generators", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          files: [{ path: directive.path, content: activeCodeContent }],
        }),
      });
      const data = await res.json();
      if (res.ok && data.success) {
        addLog(
          `Compiled format '${activeTab}' written to [${directive.path}].`,
        );
      } else {
        addLog(
          `Sync failure: Server failed write request with error: ${data.error || "Unknown Error"}`,
        );
      }
    } catch (err: any) {
      addLog(`Sync failire: ${err.message}`);
    }
  };

  // Restore Default Template
  const handleRestoreTemplate = async () => {
    try {
      addLog("Initializing backend reset payload POST /api/initialize...");
      const res = await fetch("/api/initialize", { method: "POST" });
      const data = await res.json();
      if (data.success) {
        setConfigValues({});
        setBootSuccess(true);
        setBootError("");
        addLog("Default telemetry core file re-established.");
        await loadFiles();
      }
    } catch (e: any) {
      addLog(`Initialization Failure: ${e.message}`);
    }
  };

  // Delete ZXConfig strictly to proof boot core failure trigger requested!
  const handleDeleteZXConfig = async () => {
    if (
      !window.confirm(
        "This will delete the file 'ZXConfig' on the server. The application will immediately encounter a fatal error trying to find the deleted file. Proceed?",
      )
    ) {
      return;
    }
    try {
      addLog("dispatched /api/delete-file...");
      const res = await fetch("/api/delete-file", { method: "DELETE" });
      if (res.ok) {
        setBootSuccess(false);
        setBootError(
          "Unrecoverable error: File 'ZXConfig' was purged from filesystem root.",
        );
      }
    } catch (err: any) {
      addLog(`Purge call failed: ${err.message}`);
    }
  };

  // Window depth and focus helper
  const focusWindow = (id: string) => {
    maxZIndexRef.current += 1;
    setWindows((prev) =>
      prev.map((win) =>
        win.id === id
          ? {
              ...win,
              zIndex: maxZIndexRef.current,
              isOpen: true,
              isMinimized: false,
            }
          : win,
      ),
    );
    if (id === "about") {
      fetchSysDump();
    }
  };

  const toggleWindowClose = (id: string) => {
    setWindows((prev) =>
      prev.map((win) =>
        win.id === id ? { ...win, isOpen: !win.isOpen } : win,
      ),
    );
  };

  const toggleWindowMinimize = (id: string) => {
    setWindows((prev) =>
      prev.map((win) =>
        win.id === id ? { ...win, isMinimized: !win.isMinimized } : win,
      ),
    );
  };

  const toggleWindowMaximize = (id: string, e?: React.MouseEvent) => {
    if (e) e.stopPropagation();
    setWindows((prev) =>
      prev.map((win) =>
        win.id === id ? { ...win, isMaximized: !win.isMaximized } : win,
      ),
    );
  };

  // Draggable Event Handler
  const handleHeaderMouseDown = (id: string, e: React.MouseEvent) => {
    const win = windows.find((w) => w.id === id);
    if (!win || win.isMaximized) return;

    // Bring clicked window to the absolute front
    focusWindow(id);

    setIsDragging(true);
    setDraggedWinId(id);
    setDragStart({
      x: e.clientX,
      y: e.clientY,
      winX: win.x,
      winY: win.y,
    });
    e.preventDefault();
  };

  const handleGlobalMouseMove = (e: React.MouseEvent) => {
    if (isDragging && draggedWinId && dragStart) {
      const deltaX = e.clientX - dragStart.x;
      const deltaY = e.clientY - dragStart.y;

      setWindows((prev) =>
        prev.map((win) =>
          win.id === draggedWinId
            ? {
                ...win,
                x: Math.max(
                  0,
                  Math.min(window.innerWidth - 100, dragStart.winX + deltaX),
                ),
                y: Math.max(
                  40,
                  Math.min(window.innerHeight - 80, dragStart.winY + deltaY),
                ),
              }
            : win,
        ),
      );
    } else if (isResizing && resizedWinId && resizeStart) {
      const deltaX = e.clientX - resizeStart.x;
      const deltaY = e.clientY - resizeStart.y;

      setWindows((prev) =>
        prev.map((win) =>
          win.id === resizedWinId
            ? {
                ...win,
                width: Math.max(250, resizeStart.winWidth + deltaX),
                height: Math.max(150, resizeStart.winHeight + deltaY),
              }
            : win,
        ),
      );
    }
  };

  const handleGlobalMouseUp = () => {
    setIsDragging(false);
    setDraggedWinId(null);
    setDragStart(null);
    setIsResizing(false);
    setResizedWinId(null);
    setResizeStart(null);
  };

  // Copy code generator output helper
  const copyToClipboard = (text: string, tabId: string) => {
    navigator.clipboard.writeText(text);
    setCopiedId(tabId);
    setTimeout(() => setCopiedId(null), 1500);
  };

  // Compute constraints validation
  const { errors: constraintErrors } = evaluateConstraints(
    parsedDoc,
    configValues,
  );

  // Compute dependency flow analyzer variables lists
  const getSelectedDependencies = () => {
    const opt = parsedDoc.optionsMap[dependencyTraceId];
    if (!opt) return { deps: [], depString: "None" };

    const list: string[] = [];
    const traverse = (expr: any) => {
      if (!expr) return;
      if (expr.type === "variable" && expr.name) {
        list.push(expr.name);
      }
      if (expr.left) traverse(expr.left);
      if (expr.right) traverse(expr.right);
    };

    traverse(opt.visibleWhen);
    traverse(opt.constraint);

    // Also look for group visibility constraints
    for (const group of parsedDoc.groups) {
      if (group.children.some((c) => c.id === dependencyTraceId)) {
        traverse(group.visibleWhen);
      }
    }

    const unique = Array.from(new Set(list));
    return {
      deps: unique,
      depString:
        unique.length > 0
          ? unique.join(", ")
          : "Self-contained / No requirements",
    };
  };

  const getSelectedDependents = () => {
    const list: string[] = [];
    const checkExpr = (expr: any, searchId: string): boolean => {
      if (!expr) return false;
      if (expr.type === "variable" && expr.name === searchId) return true;
      return checkExpr(expr.left, searchId) || checkExpr(expr.right, searchId);
    };

    for (const id of Object.keys(parsedDoc.optionsMap)) {
      if (id === dependencyTraceId) continue;
      const otherOpt = parsedDoc.optionsMap[id];

      // Check options visibility or constraints rules
      if (
        checkExpr(otherOpt.visibleWhen, dependencyTraceId) ||
        checkExpr(otherOpt.constraint, dependencyTraceId)
      ) {
        list.push(id);
      }
    }

    // Check nested parent group bounds declarations rules
    for (const group of parsedDoc.groups) {
      if (checkExpr(group.visibleWhen, dependencyTraceId)) {
        group.children.forEach((c) => {
          if (c.id !== dependencyTraceId) list.push(c.id);
        });
      }
    }

    const unique = Array.from(new Set(list));
    return {
      dependents: unique,
      depString:
        unique.length > 0 ? unique.join(", ") : "None (Is safe to toggle)",
    };
  };

  const traceInfo = getSelectedDependencies();
  const traceDeps = getSelectedDependents();

  // Generated outputs content
  const activeCodeContent = generateFormat(activeTab, parsedDoc, configValues);

  // Main UI render
  return (
    <div
      className="w-screen h-screen select-none select-text relative bg-[#121212] flex flex-col font-mono text-[#e0e0e0] overflow-hidden"
      onMouseMove={handleGlobalMouseMove}
      onMouseUp={handleGlobalMouseUp}
    >
      {/* Dynamic Desktop Header Bar */}
      <header className="h-9 bg-black border-b border-zinc-800 px-4 flex items-center justify-between shrink-0 select-none z-[100]">
        <div className="flex items-center space-x-4">
          <div className="flex items-center space-x-1.5">
            <div className="w-2.5 h-2.5 bg-neutral-100 animate-pulse"></div>
            <span className="font-bold tracking-tight text-white text-sm">
              ZXConfig
            </span>
          </div>

          <div className="h-4 border-r border-zinc-800"></div>

          {/* Quick task-menu toggles */}
          <nav className="flex space-x-3 text-xs">
            <button
              onClick={() => focusWindow("editor")}
              className={`hover:text-white transition-colors flex items-center space-x-1 ${windows.find((w) => w.id === "editor" && w.isOpen && !w.isMinimized) ? "text-white underline underline-offset-4 font-bold" : "text-zinc-500"}`}
            >
              <FileCode className="w-3.5 h-3.5" />
              <span>[Editor]</span>
            </button>
            <button
              onClick={() => focusWindow("gui")}
              className={`hover:text-white transition-colors flex items-center space-x-1 ${windows.find((w) => w.id === "gui" && w.isOpen && !w.isMinimized) ? "text-white underline underline-offset-4 font-bold" : "text-zinc-500"}`}
            >
              <Sliders className="w-3.5 h-3.5" />
              <span>[Parameters]</span>
            </button>
            <button
              onClick={() => focusWindow("compiler")}
              className={`hover:text-white transition-colors flex items-center space-x-1 ${windows.find((w) => w.id === "compiler" && w.isOpen && !w.isMinimized) ? "text-white underline underline-offset-4 font-bold" : "text-zinc-500"}`}
            >
              <FileText className="w-3.5 h-3.5" />
              <span>[Compiler]</span>
            </button>
            <button
              onClick={() => focusWindow("ast")}
              className={`hover:text-white transition-colors flex items-center space-x-1 ${windows.find((w) => w.id === "ast" && w.isOpen && !w.isMinimized) ? "text-white underline underline-offset-4 font-bold" : "text-zinc-500"}`}
            >
              <Layers className="w-3.5 h-3.5" />
              <span>[Diagnostics]</span>
            </button>
            <button
              onClick={() => focusWindow("fs")}
              className={`hover:text-white transition-colors flex items-center space-x-1 ${windows.find((w) => w.id === "fs" && w.isOpen && !w.isMinimized) ? "text-white underline underline-offset-4 font-bold" : "text-zinc-500"}`}
            >
              <Terminal className="w-3.5 h-3.5" />
              <span>[Host-Monitor]</span>
            </button>
            <button
              onClick={() => focusWindow("about")}
              className={`hover:text-white transition-colors flex items-center space-x-1 ${windows.find((w) => w.id === "about" && w.isOpen && !w.isMinimized) ? "text-white underline underline-offset-4 font-bold" : "text-zinc-500"}`}
            >
              <HelpCircle className="w-3.5 h-3.5" />
              <span>[About]</span>
            </button>
          </nav>
        </div>

        {/* Real-time System state Indicators */}
        <div className="flex items-center space-x-4 text-xs text-zinc-400">
          <div className="flex items-center space-x-2 bg-zinc-900 border border-zinc-800 px-2 py-0.5 whitespace-nowrap">
            <span className="text-zinc-500">Parameters:</span>
            <span className="text-white font-bold">
              {Object.keys(parsedDoc.optionsMap).length}
            </span>
          </div>

          <div className="flex items-center space-x-1">
            <Clock className="w-3.5 h-3.5 text-zinc-500" />
            <span className="text-zinc-300 font-bold whitespace-nowrap">
              {currentTime || "Loading UTC..."}
            </span>
          </div>
        </div>
      </header>

      {/* Boot Interruption Crash Interface (File ZXConfig is not present) */}
      {bootSuccess === false && (
        <div className="absolute inset-0 top-9 bg-[#1a0505] text-white z-[9999] flex flex-col justify-between p-12 select-text font-mono overflow-auto">
          <div className="max-w-4xl space-y-8">
            <div className="bg-[#5c0d0d] text-white border border-[#ff3838] p-4 font-bold flex items-center space-x-3 text-lg animate-pulse">
              <AlertTriangle className="w-7 h-7 stroke-[2]" />
              <span>[! System Internal Error !]</span>
            </div>

            <div className="space-y-4">
              <h1 className="text-3xl font-extrabold text-[#ff4f4f] tracking-tight">
                Unresolved Fatal Failure
              </h1>
              <p className="text-zinc-300 text-sm leading-relaxed max-w-2xl bg-black p-4 border border-zinc-800 font-mono">
                {bootError}
                <br />
                <br />
                System call:{" "}
                <code className="text-white font-bold">
                  fs.readFileSync(path.join(cwd, "ZXConfig"))
                </code>{" "}
                returned error flag:{" "}
                <code className="text-[#ff3838]">ENOENT</code>.
              </p>
            </div>

            <div className="bg-[#111] border border-zinc-800 p-6 space-y-4 max-w-xl">
              <span className="text-zinc-400 text-xs font-bold block">
                [DIAGNOSTICS & HARDWARE CONTRACT LOG]
              </span>
              <ul className="text-xs space-y-2 text-zinc-300 font-bold">
                <li>
                  ● Looking for host schema manifest at:{" "}
                  <span className="text-zinc-500">ROOT/ZXConfig</span>
                </li>
                <li>
                  ● Error Outcome: Checked and failed because of file deletion
                  or missing asset.
                </li>
                <li>
                  ● ZXConfig mandates complete presence of 'ZXConfig' on server
                  launch.
                </li>
              </ul>
            </div>

            <div className="pt-4 flex flex-wrap gap-4 items-center">
              <button
                onClick={handleRestoreTemplate}
                className="bg-[#22c55e] hover:bg-[#16a34a] text-black font-extrabold px-6 py-3 border-2 border-black transition-all shadow-[4px_4px_0px_0px_rgba(255,255,255,1)] active:translate-x-1 active:translate-y-1 active:shadow-none uppercase text-xs tracking-wider"
              >
                Assemble Default "ZXConfig" Manifest file
              </button>

              <button
                onClick={checkBootStatus}
                className="bg-black hover:bg-zinc-900 text-white font-extrabold px-6 py-3 border border-zinc-700 hover:border-white transition-all text-xs tracking-wider uppercase flex items-center space-x-2"
              >
                <RefreshCw className="w-4 h-4" />
                <span>Re-poll Host Link Status</span>
              </button>
            </div>
          </div>

          <div className="pt-12 text-zinc-600 text-xs text-left">
            ZXConfig engine
          </div>
        </div>
      )}

      {/* Kconfig-inspired Cache loading prompt (missing or partial cache) */}
      {bootSuccess === true && cachePromptPending && (
        <div className="absolute inset-0 bg-black/90 backdrop-blur-md z-[9999] flex items-center justify-center p-4">
          <div className="max-w-xl w-full bg-[#111115] border-2 border-zinc-700 p-6 rounded-xl shadow-2xl space-y-6">
            <div className="flex items-center space-x-3 text-yellow-500 border-b border-zinc-800 pb-3">
              <Sliders className="w-5 h-5 shrink-0" />
              <span className="text-xs font-black tracking-widest uppercase font-mono">
                ZXConfig cache manager
              </span>
            </div>

            <div className="space-y-2">
              <h2 className="text-base font-extrabold text-white tracking-tight">
                Previous cache detected
              </h2>
              <p className="text-zinc-400 text-xs leading-relaxed font-sans">
                The specification file{" "}
                <code className="text-zinc-200 font-bold px-1.5 py-0.5 bg-black rounded font-mono">
                  ZXConfig
                </code>{" "}
                loaded successfully. However, the configuration value cache is
                missing or only partially constructed. How would you like to
                initialize the configurations?
              </p>
            </div>

            <div className="bg-[#09090c] p-4 rounded border border-zinc-800 space-y-2.5 text-[11px] font-mono leading-relaxed">
              <div className="flex justify-between border-b border-zinc-900 pb-1.5 text-zinc-500 font-bold uppercase">
                <span>Metric</span>
                <span>Status</span>
              </div>
              <div className="flex justify-between">
                <span className="text-zinc-400">Main Specification Spec:</span>
                <span className="text-emerald-500 font-bold">
                  Loaded (ZXConfig)
                </span>
              </div>
              <div className="flex justify-between">
                <span className="text-zinc-400">
                  Total Specification Option Nodes:
                </span>
                <span className="text-white font-bold">
                  {Object.keys(parsedDoc.optionsMap).length} Options
                </span>
              </div>
              <div className="flex justify-between">
                <span className="text-zinc-400">
                  Cached Values (ZXConfig.values):
                </span>
                <span className="text-yellow-500 font-bold">
                  {Object.keys(staleCacheValues).length === 0
                    ? "Empty / Missing"
                    : `${Object.keys(staleCacheValues).length} keys (Stale / Partially Loaded)`}
                </span>
              </div>
            </div>

            <div className="space-y-3 pt-2">
              <button
                onClick={handleLoadFreshDefaults}
                className="w-full bg-white hover:bg-zinc-250 text-black py-3 px-4 font-bold border border-black transition-colors rounded-lg text-xs uppercase flex items-center justify-between cursor-pointer"
              >
                <span>Load Fresh Default Configuration (Factory Preset)</span>
                <span className="text-[9px] bg-zinc-200 px-2 py-0.5 rounded text-zinc-800 font-black">
                  RECOMMENDED
                </span>
              </button>

              <button
                onClick={handleRestoreStaleCache}
                className="w-full bg-[#18181c] hover:bg-zinc-900 text-zinc-300 py-3 px-4 font-semibold border border-zinc-800 transition-colors rounded-lg text-xs uppercase flex items-center justify-between cursor-pointer"
              >
                <span>
                  Keep partial/stale cache & pad missing options with defaults
                </span>
                <span className="text-[9px] bg-zinc-800 px-2 py-0.5 rounded text-zinc-400 font-black">
                  STALE CACHE
                </span>
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Main Window Workspace Canvas Wrapper */}
      <div
        className="flex-1 relative bg-[#0f0f12] overflow-hidden"
        style={{
          backgroundSize: "16px 16px",
          backgroundImage: "radial-gradient(circle, #222 1px, transparent 1px)",
        }}
      >
        {/* Quick Restore Toolbar for Minimized / Closed elements */}
        {windows.some((w) => !w.isOpen || w.isMinimized) && (
          <div className="absolute right-6 top-6 bg-black/95 border border-zinc-800 p-3 flex flex-col space-y-2 z-[90] max-w-[220px] shadow-2xl rounded-xl">
            <span className="text-[10px] text-zinc-500 font-bold tracking-widest block border-b border-zinc-800 pb-1 mb-1 uppercase">
              DOCK MANAGER
            </span>
            {windows.map((win) => {
              if (win.isOpen && !win.isMinimized) return null;
              return (
                <button
                  key={win.id}
                  onClick={() => focusWindow(win.id)}
                  className="w-full text-left font-mono text-zinc-400 hover:text-white text-xs py-1.5 px-2 border border-zinc-900 hover:border-zinc-700 bg-zinc-950 flex items-center justify-between rounded-lg transition-colors hover:bg-zinc-900/40"
                >
                  <span className="truncate pr-1.5">{win.title}</span>
                  <span className="text-[9px] bg-zinc-800 px-1.5 py-0.5 rounded text-zinc-300">
                    RESTORE
                  </span>
                </button>
              );
            })}
          </div>
        )}

        {/* Dynamic Windows Loop */}
        {windows.map((win) => {
          if (!win.isOpen) return null;

          const isEditor = win.id === "editor";
          const isGui = win.id === "gui";
          const isCompiler = win.id === "compiler";
          const isAst = win.id === "ast";
          const isFs = win.id === "fs";

          return (
            <div
              key={win.id}
              onClick={() => focusWindow(win.id)}
              className={`absolute border flex flex-col bg-[#0c0c0f] select-none transition-all duration-75 ${
                win.isMinimized ? "invisible" : ""
              } ${
                win.isMaximized
                  ? "inset-0 top-0 left-0 right-0 bottom-0 w-full h-full z-[80] rounded-none border-0"
                  : "rounded-xl border-[#2c2c34] shadow-[0_24px_70px_rgba(0,0,0,0.7)] overflow-hidden"
              }`}
              style={
                win.isMaximized
                  ? { transform: "none" }
                  : {
                      left: `${win.x}px`,
                      top: `${win.y}px`,
                      width: `${win.width}px`,
                      height: `${win.height}px`,
                      zIndex: win.zIndex,
                    }
              }
            >
              {/* Window TitleBar Drag Header Area (macOS styled) */}
              <div
                onMouseDown={(e) => handleHeaderMouseDown(win.id, e)}
                onDoubleClick={(e) => toggleWindowMaximize(win.id)}
                className={`h-9 bg-[#19191d] text-[#e0e0e0] px-3.5 flex items-center justify-between cursor-move shrink-0 select-none border-b border-[#242429] md:text-sm text-xs font-semibold ${
                  win.isMaximized ? "rounded-none" : "rounded-t-xl"
                }`}
              >
                {/* Left side: macOS traffic lights controls */}
                <div className="flex items-center space-x-3">
                  <div className="flex items-center space-x-1.5 mr-5 group/mac">
                    {/* Red button (Close) */}
                    <button
                      onClick={(e) => {
                        e.stopPropagation();
                        toggleWindowClose(win.id);
                      }}
                      title="Close"
                      className="w-3.5 h-3.5 rounded-full bg-[#ff5f56] hover:bg-[#ff5f56]/95 border border-[#e0443e] cursor-pointer flex items-center justify-center text-[9px] text-[#4c0002] focus:outline-none transition-colors relative"
                    >
                      <span className="opacity-0 group-hover/mac:opacity-100 absolute inset-0 flex items-center justify-center font-sans font-extrabold leading-none select-none text-[8.5px] -mt-[0.5px]">
                        ×
                      </span>
                    </button>
                    {/* Yellow button (Minimize) */}
                    <button
                      onClick={(e) => {
                        e.stopPropagation();
                        toggleWindowMinimize(win.id);
                      }}
                      title="Minimize"
                      className="w-3.5 h-3.5 rounded-full bg-[#ffbd2e] hover:bg-[#ffbd2e]/95 border border-[#dea123] cursor-pointer flex items-center justify-center text-[11px] text-[#5c3e00] focus:outline-none transition-colors relative"
                    >
                      <span className="opacity-0 group-hover/mac:opacity-100 absolute inset-0 flex items-center justify-center font-sans font-extrabold leading-none select-none text-[11px] -mt-[1.5px]">
                        –
                      </span>
                    </button>
                    {/* Green button (Maximize) */}
                    <button
                      onClick={(e) => {
                        e.stopPropagation();
                        toggleWindowMaximize(win.id);
                      }}
                      title="Maximize"
                      className="w-3.5 h-3.5 rounded-full bg-[#27c93f] hover:bg-[#27c93f]/95 border border-[#1aab29] cursor-pointer flex items-center justify-center text-[8px] text-[#014d06] focus:outline-none transition-colors relative"
                    >
                      <span className="opacity-0 group-hover/mac:opacity-100 absolute inset-0 flex items-center justify-center font-sans font-extrabold leading-none select-none text-[8.5px] -mt-[0.5px]">
                        +
                      </span>
                    </button>
                  </div>
                  <span className="truncate font-sans font-semibold text-xs text-[#e1e2e6] tracking-wide select-none">
                    {win.title}
                  </span>
                </div>

                {/* Right side window context tags */}
                <div className="flex items-center space-x-2 text-[10px] text-zinc-500 font-mono select-none">
                  {isEditor && (
                    <span className="bg-[#242429] px-2 py-0.5 rounded border border-[#2f2f35]">
                      ZXDSL Editor
                    </span>
                  )}
                  {isGui && (
                    <span className="bg-[#242429] px-2 py-0.5 rounded border border-[#2f2f35]">
                      Parameters
                    </span>
                  )}
                  {isCompiler && (
                    <span className="bg-[#242429] px-2 py-0.5 rounded border border-[#2f2f35]">
                      Compiler
                    </span>
                  )}
                  {isFs && (
                    <span className="bg-[#242429] px-2 py-0.5 rounded border border-[#2f2f35]">
                      Monitor
                    </span>
                  )}
                  {win.id === "about" && (
                    <span className="bg-[#242429] px-2 py-0.5 rounded border border-[#2f2f35]">
                      Diagnostics
                    </span>
                  )}
                </div>
              </div>

              {/* Window Body Container */}
              <div
                className={`flex-1 p-4 ${isEditor ? "overflow-hidden" : "overflow-auto"} flex flex-col min-h-0 select-text bg-[#0b0b0d]`}
              >
                {/* 1. SCHEMA EDITOR WINDOW INTERNAL */}
                {isEditor && (
                  <div className="flex-1 flex flex-col min-h-0">
                    <div className="flex items-center justify-between text-xs pb-3 border-b border-zinc-800 text-zinc-400 mb-3 shrink-0">
                      <div className="flex items-center space-x-2">
                        <span className="text-zinc-500 font-bold">
                          ACTIVE FILE:
                        </span>
                        <span className="text-white font-extrabold border border-zinc-700 px-1 bg-zinc-900">
                          {currentFilename}
                        </span>
                      </div>
                      <div className="flex items-center space-x-3">
                        <label className="flex items-center space-x-1.5 cursor-pointer">
                          <input
                            type="checkbox"
                            checked={isAutoSaving}
                            onChange={(e) => setIsAutoSaving(e.target.checked)}
                            className="accent-white cursor-pointer"
                          />
                          <span className="text-zinc-400 hover:text-white select-none">
                            Auto-Save Scheme
                          </span>
                        </label>
                        <button
                          onClick={() =>
                            handleFileChange(
                              currentFilename,
                              filesMap[currentFilename] || "",
                            )
                          }
                          className="bg-white hover:bg-zinc-300 text-black text-[10px] px-2 py-0.5 border border-black font-extrabold hover:translate-y-[1px]"
                        >
                          FORCE COMPILE SYSTEM
                        </button>
                      </div>
                    </div>

                    {/* Integrated Sidebar File Explorer & Side-by-Side Live Highlights Editor Grid */}
                    <div className="flex-1 flex min-h-0 border border-zinc-850 rounded bg-black">
                      {/* Left Sidebar: Specs file explorer */}
                      <div className="w-44 bg-zinc-950/90 border-r border-[#1a1a1a] flex flex-col shrink-0 min-h-0 text-[11px]">
                        <div className="p-2 border-b border-zinc-900 flex items-center justify-between font-bold text-zinc-500 select-none bg-black">
                          <span>Spec Files</span>
                          <button
                            onClick={() => {
                              const name = window.prompt(
                                "Enter new DSL spec filename (must end with .zxdsl):",
                              );
                              if (name) {
                                if (!name.endsWith(".zxdsl")) {
                                  alert(
                                    "Error: Filename must end with .zxdsl suffix.",
                                  );
                                  return;
                                }
                                handleAddNewFile(name);
                              }
                            }}
                            className="text-white hover:text-red-400 font-black px-1 text-xs"
                            title="Create new spec file"
                          >
                            [+]
                          </button>
                        </div>

                        <div className="flex-grow overflow-y-auto space-y-0.5 p-1">
                          {Object.keys(filesMap).map((fn) => (
                            <div
                              key={fn}
                              className={`flex items-center justify-between p-1.5 cursor-pointer font-mono group ${
                                fn === currentFilename
                                  ? "bg-white text-black font-extrabold border border-black"
                                  : "text-zinc-400 bg-zinc-900/10 hover:bg-zinc-900/60"
                              }`}
                              onClick={() => setCurrentFilename(fn)}
                            >
                              <span className="truncate flex items-center space-x-1">
                                <span className="text-zinc-500">
                                  {fn === "ZXConfig" ? "⚓" : "📄"}
                                </span>
                                <span className="truncate">{fn}</span>
                              </span>

                              {fn !== "ZXConfig" && (
                                <button
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    if (
                                      window.confirm(
                                        `Are you sure you want to delete file '${fn}'?`,
                                      )
                                    ) {
                                      handleDeleteFileSpec(fn);
                                    }
                                  }}
                                  className={`font-mono font-black text-[9px] px-1 select-none focus:outline-none opacity-0 group-hover:opacity-100 ${
                                    fn === currentFilename
                                      ? "text-red-650 hover:bg-black/10"
                                      : "text-red-500 hover:text-red-300"
                                  }`}
                                >
                                  ×
                                </button>
                              )}
                            </div>
                          ))}
                        </div>
                      </div>

                      {/* Right Panel Workspace: Custom Editor input + Live dynamic code highlighter */}
                      <div className="flex-1 flex min-h-0 bg-[#060608] relative">
                        {/* Line Numbers Gutter */}
                        <div
                          ref={gutterRef}
                          className="w-10 bg-[#09090c] border-r border-[#151515] select-none text-[9px] text-[#555] font-mono text-right overflow-hidden shrink-0"
                          style={{
                            paddingTop: "12px",
                            paddingBottom: "12px",
                          }}
                        >
                          {Array.from({
                            length: Math.max(
                              15,
                              (filesMap[currentFilename] || "").split("\n")
                                .length,
                            ),
                          }).map((_, index) => (
                            <div
                              key={index}
                              className="h-[1.4rem] leading-[1.4rem] pr-2.5"
                            >
                              {index + 1}
                            </div>
                          ))}
                        </div>

                        {/* Relative wrapper for overlaid Editor and pre syntax layer */}
                        <div className="flex-1 relative overflow-hidden h-full">
                          {/* syntax overlay layer beneath */}
                          <pre
                            ref={preRef}
                            className="absolute inset-0 m-0 font-mono text-[11px] leading-[1.4rem] whitespace-pre overflow-hidden pointer-events-none text-zinc-400 z-0 select-none bg-transparent"
                            style={{
                              fontFamily:
                                "var(--font-mono), ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace",
                              padding: "12px",
                            }}
                          >
                            {(filesMap[currentFilename] || "")
                              .split("\n")
                              .map((line, idx) => (
                                <div
                                  key={idx}
                                  className="min-h-[1.4rem] leading-[1.4rem] h-[1.4rem] whitespace-pre"
                                >
                                  {highlightZXDSL(line)}
                                </div>
                              ))}
                          </pre>

                          {/* interactive textarea on top with transparent text but visible caret */}
                          <textarea
                            ref={textareaRef}
                            value={filesMap[currentFilename] || ""}
                            onChange={(e) =>
                              handleFileChange(currentFilename, e.target.value)
                            }
                            onScroll={handleScroll}
                            placeholder="# Define custom ZXDSL configurations here..."
                            spellCheck={false}
                            className="absolute inset-0 m-0 bg-transparent text-transparent caret-white outline-none border-none font-mono text-[11px] leading-[1.4rem] resize-none overflow-auto whitespace-pre z-10 w-full h-full select-text"
                            style={{
                              fontFamily:
                                "var(--font-mono), ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace",
                              padding: "12px",
                              WebkitTextFillColor: "transparent",
                            }}
                          />
                        </div>
                      </div>
                    </div>

                    {/* Real-time AST Parser Alerts Box */}
                    <div className="mt-3 shrink-0">
                      <div className="bg-zinc-950 border border-zinc-800 p-2.5 max-h-[110px] overflow-auto text-[11px] font-mono">
                        <div className="flex items-center justify-between text-zinc-500 border-b border-zinc-900 pb-1 mb-1 font-bold">
                          <span>Interpreter Logs</span>
                          <span
                            className={`${parseErrors.length > 0 ? "text-red-500" : "text-emerald-500"} font-black`}
                          >
                            {parseErrors.length > 0
                              ? `[!] ${parseErrors.length} ERROR(S) FOUND`
                              : "[✓] SYNTAX VALID"}
                          </span>
                        </div>

                        {parseErrors.length === 0 ? (
                          <div className="text-zinc-400 flex items-center space-x-1 py-1">
                            <span className="text-emerald-500 font-bold">
                              ✓
                            </span>
                            <span>ZXDSL syntactic parsing loop passed.</span>
                          </div>
                        ) : (
                          <div className="space-y-1">
                            {parseErrors.map((err, idx) => (
                              <div
                                key={idx}
                                className={`flex items-start space-x-2 py-0.5 leading-snug cursor-pointer hover:bg-zinc-900 ${
                                  err.severity === "error"
                                    ? "text-red-400"
                                    : "text-yellow-400"
                                }`}
                              >
                                <span className="font-bold shrink-0">
                                  [{err.severity === "error" ? "ERR" : "WRN"} Ln{" "}
                                  {err.line}, Col {err.column}]:
                                </span>
                                <span>{err.message}</span>
                              </div>
                            ))}
                          </div>
                        )}
                      </div>
                    </div>
                  </div>
                )}

                {/* 2. INTERACTIVE CONFIG PANEL (GUI) WINDOW INTERNAL */}
                {isGui && (
                  <div className="flex-1 flex flex-col min-h-0">
                    <div className="flex items-center justify-between text-xs pb-3 border-b border-zinc-800 text-zinc-400 mb-4 shrink-0">
                      <div className="flex items-center space-x-2">
                        <span className="text-zinc-500 font-bold">
                          Target Engine:
                        </span>
                        <span className="text-white font-extrabold border border-zinc-700 px-1 bg-zinc-900">
                          Active Evaluator
                        </span>
                      </div>
                      <div className="text-zinc-500 font-semibold">
                        [Double click variable to inspect AST]
                      </div>
                    </div>

                    {/* Form Layout rendering parsed blocks */}
                    {parsedDoc.groups.length === 0 ? (
                      <div className="flex-1 flex flex-col items-center justify-center p-8 text-center bg-zinc-950 border border-dashed border-zinc-800">
                        <AlertTriangle className="w-10 h-10 text-yellow-500 mb-2" />
                        <h2 className="text-sm font-bold text-white mb-1">
                          No Option Group Parsed
                        </h2>
                        <p className="text-xs text-zinc-500 max-w-sm">
                          Please resolve active compiler errors in the Schema
                          Editor or click 'Restore Template' to stand up
                          configuration elements.
                        </p>
                      </div>
                    ) : (
                      <div className="flex-1 space-y-6 overflow-y-auto pr-1">
                        {/* Constraints violation status box */}
                        {constraintErrors.length > 0 && (
                          <div className="bg-[#1f1911] border border-yellow-600/80 p-3.5 space-y-1.5 rounded-sm shrink-0">
                            <div className="flex items-center space-x-2 font-bold text-yellow-500 text-xs">
                              <AlertTriangle className="w-4 h-4 shrink-0" />
                              <span>
                                Config Exception: {constraintErrors.length}{" "}
                                Triggered Warnings
                              </span>
                            </div>
                            <div className="text-[11px] leading-relaxed text-zinc-300 pl-6 space-y-1.5 list-disc select-text">
                              {constraintErrors.map((err, i) => (
                                <div
                                  key={i}
                                  className="text-yellow-400/90 font-mono"
                                >
                                  • {err}
                                </div>
                              ))}
                            </div>
                          </div>
                        )}

                        {parsedDoc.groups.map((group) => {
                          // Check group visibility condition
                          const groupVisible =
                            !group.visibleWhen ||
                            evaluateExpression(
                              group.visibleWhen,
                              configValues,
                              parsedDoc.optionsMap,
                            );

                          return (
                            <div
                              key={group.id}
                              className={`border ${
                                groupVisible
                                  ? "border-zinc-800 bg-zinc-950/20"
                                  : "border-zinc-950 bg-zinc-950/10 opacity-30 select-none"
                              } p-4 rounded-sm relative`}
                            >
                              {/* Parent Invisible Strike Overlay */}
                              {!groupVisible && (
                                <div className="absolute inset-0 bg-black/60 backdrop-blur-[1px] flex items-center justify-center pointer-events-none z-10">
                                  <span className="text-[10px] tracking-widest text-[#ff4f4f] border border-[#ff4f4f] px-2 py-0.5 bg-black font-extrabold uppercase">
                                    [GROUP BYTREATED BY DEPS]
                                  </span>
                                </div>
                              )}

                              {/* Group Header title */}
                              <div className="flex items-center justify-between border-b border-zinc-800 pb-2 mb-4">
                                <h3 className="text-xs font-black uppercase text-white tracking-widest flex items-center space-x-1.5">
                                  <span>{group.label}</span>
                                </h3>
                                <span className="text-[9px] text-zinc-500 font-mono">
                                  ID: {group.id}
                                </span>
                              </div>

                              {/* Group Options loop */}
                              <div className="space-y-4">
                                {group.children.length === 0 ? (
                                  <div className="text-xs text-zinc-650 italic pl-1">
                                    No options defined inside this scope block.
                                  </div>
                                ) : (
                                  group.children.map((opt) => {
                                    const optVisible = isNodeVisible(
                                      opt.id,
                                      parsedDoc,
                                      configValues,
                                    );
                                    const rawVal =
                                      configValues[opt.id] !== undefined
                                        ? configValues[opt.id]
                                        : opt.defaultValue;

                                    // Constraint validation bounds
                                    let isConstraintFailed = false;
                                    if (optVisible && opt.constraint) {
                                      isConstraintFailed = !evaluateExpression(
                                        opt.constraint,
                                        configValues,
                                        parsedDoc.optionsMap,
                                      );
                                    }

                                    return (
                                      <div
                                        key={opt.id}
                                        onDoubleClick={() => {
                                          setDependencyTraceId(opt.id);
                                          focusWindow("ast");
                                        }}
                                        className={`p-3 border select-text relative transition-colors ${
                                          !optVisible
                                            ? "border-zinc-950 bg-[#09090a]/50 text-zinc-600 cursor-not-allowed"
                                            : isConstraintFailed
                                              ? "border-[#ff3838]/70 bg-[#140606] shadow-inner"
                                              : "border-zinc-900 bg-zinc-950/80 hover:border-zinc-700"
                                        }`}
                                      >
                                        {/* INACTIVE LABEL STENCIL */}
                                        {!optVisible && (
                                          <div className="absolute inset-0 bg-black/40 flex items-center justify-center pointer-events-none z-[2]">
                                            <span className="text-[9px] text-zinc-500 uppercase tracking-widest bg-zinc-950 px-1.5 py-0.5 border border-zinc-900 font-bold">
                                              INACTIVE
                                            </span>
                                          </div>
                                        )}

                                        <div className="flex items-start justify-between">
                                          <div className="space-y-1 flex-1 min-w-0 pr-4">
                                            <div className="flex items-center space-x-2">
                                              <span
                                                className={`text-xs font-extrabold ${optVisible ? "text-white" : "text-zinc-600 line-through"}`}
                                              >
                                                {opt.label}
                                              </span>
                                              <span className="text-[9px] font-mono text-zinc-500 bg-[#0f0f12] px-1 ring-1 ring-zinc-800">
                                                {opt.id}
                                              </span>
                                            </div>
                                            {opt.description && (
                                              <p
                                                className={`text-[10.5px] leading-relaxed ${optVisible ? "text-zinc-400" : "text-zinc-700 font-semibold"}`}
                                              >
                                                {opt.description}
                                              </p>
                                            )}
                                          </div>

                                          {/* CONFIG FORM INPUTS RENDERERS */}
                                          <div className="shrink-0 flex items-center justify-end min-w-[140px] z-[5]">
                                            {/* BOOLEAN SWITCHES TYPE */}
                                            {opt.type === "bool" && (
                                              <button
                                                disabled={!optVisible}
                                                onClick={() =>
                                                  handleConfigValueChange(
                                                    opt.id,
                                                    !rawVal,
                                                  )
                                                }
                                                className={`w-14 py-1 border font-bold text-[10px] tracking-widest select-none ${
                                                  !optVisible
                                                    ? "border-zinc-900 text-zinc-700"
                                                    : rawVal
                                                      ? "bg-white text-black border-black hover:bg-zinc-300"
                                                      : "bg-zinc-900 text-zinc-450 border-zinc-700 hover:text-white"
                                                }`}
                                              >
                                                {rawVal ? "[ON]" : "[OFF]"}
                                              </button>
                                            )}

                                            {/* INT TYPE SLIDERS */}
                                            {opt.type === "int" && (
                                              <div className="flex items-center space-x-2 text-xs w-full max-w-[180px]">
                                                <input
                                                  type="range"
                                                  min={
                                                    opt.range ? opt.range[0] : 0
                                                  }
                                                  max={
                                                    opt.range
                                                      ? opt.range[1]
                                                      : 10000
                                                  }
                                                  disabled={!optVisible}
                                                  value={
                                                    typeof rawVal === "number"
                                                      ? rawVal
                                                      : parseInt(
                                                          String(rawVal),
                                                          10,
                                                        ) || 0
                                                  }
                                                  onChange={(e) =>
                                                    handleConfigValueChange(
                                                      opt.id,
                                                      parseInt(
                                                        e.target.value,
                                                        10,
                                                      ),
                                                    )
                                                  }
                                                  className="w-full h-1 accent-black bg-zinc-700 cursor-pointer disabled:opacity-20 disabled:cursor-not-allowed"
                                                />
                                                <span className="font-bold text-white bg-zinc-900 border border-zinc-800 px-1 py-0.5 min-w-[45px] text-center text-[10px]">
                                                  {rawVal}
                                                </span>
                                              </div>
                                            )}

                                            {/* SELECT DROP-DOWNS LISTBOX */}
                                            {opt.type === "select" && (
                                              <div className="flex flex-col space-y-1 w-full max-w-[180px]">
                                                {opt.values?.map((pval) => (
                                                  <button
                                                    key={pval}
                                                    disabled={!optVisible}
                                                    onClick={() =>
                                                      handleConfigValueChange(
                                                        opt.id,
                                                        pval,
                                                      )
                                                    }
                                                    className={`text-[9.5px] font-bold text-left px-2 py-0.5 border ${
                                                      !optVisible
                                                        ? "border-zinc-900 text-zinc-700"
                                                        : rawVal === pval
                                                          ? "bg-white text-black border-black font-extrabold"
                                                          : "border-zinc-800 text-zinc-400 bg-zinc-950 hover:bg-zinc-900"
                                                    }`}
                                                  >
                                                    {rawVal === pval
                                                      ? "• "
                                                      : "  "}
                                                    {pval}
                                                  </button>
                                                ))}
                                              </div>
                                            )}

                                            {/* STRING CHEX SYSTEM */}
                                            {opt.type === "string" && (
                                              <input
                                                type="text"
                                                disabled={!optVisible}
                                                value={String(rawVal)}
                                                onChange={(e) =>
                                                  handleConfigValueChange(
                                                    opt.id,
                                                    e.target.value,
                                                  )
                                                }
                                                className="w-full text-xs font-mono bg-zinc-950 border border-zinc-800 hover:border-zinc-700 focus:border-white focus:outline-none p-1 text-white disabled:opacity-30"
                                              />
                                            )}
                                          </div>
                                        </div>

                                        {/* Constraint line-bound Warning Trigger info */}
                                        {optVisible && isConstraintFailed && (
                                          <div className="mt-2.5 bg-[#5e1919]/50 border border-[#f85149]/50 px-2 py-1 flex items-center space-x-1.5 text-[9.5px] text-red-300">
                                            <AlertTriangle className="w-3.5 h-3.5 text-[#f85149] shrink-0" />
                                            <span>
                                              Rule Exception: Requires strict
                                              evaluation [
                                              <code className="text-white font-bold">
                                                {opt.constraint
                                                  ? opt.constraint.name || "..."
                                                  : "..."}
                                              </code>
                                              ] to be met.
                                            </span>
                                          </div>
                                        )}
                                      </div>
                                    );
                                  })
                                )}
                              </div>
                            </div>
                          );
                        })}
                      </div>
                    )}
                  </div>
                )}

                {/* 3. COMPILER & EXPORT COMPILATION WINDOW INTERNAL */}
                {isCompiler && (
                  <div className="flex-1 flex flex-col min-h-0">
                    {/* Tab bars selector */}
                    <div className="flex items-center space-x-1 bg-zinc-950 border-b border-zinc-800 pb-2 mb-3 shrink-0 col-span-2">
                      {[
                        { id: "c", label: "config.h (C Header)" },
                        { id: "rust", label: "config.rs (Rust Static)" },
                        { id: "makefile", label: "config.mk (Makefile)" },
                        { id: "env", label: ".env (Keyval Env)" },
                      ].map((tab) => (
                        <button
                          key={tab.id}
                          onClick={() => setActiveTab(tab.id as any)}
                          className={`text-[10px] font-bold px-3 py-1 border ${
                            activeTab === tab.id
                              ? "bg-white text-black border-black font-extrabold"
                              : "border-zinc-900 text-zinc-400 bg-[#0c0c0e] hover:bg-zinc-900 hover:text-white"
                          }`}
                        >
                          {tab.label}
                        </button>
                      ))}
                    </div>

                    {/* Integrated dynamic compile specifications */}
                    {parsedDoc.generates && parsedDoc.generates.length > 0 ? (
                      <div className="bg-[#0c0c0f] border border-zinc-900 p-2.5 rounded-sm text-[11px] mb-3 shrink-0">
                        <span className="text-zinc-500 font-bold block mb-1">
                          Parsed Export targets:
                        </span>
                        <div className="flex flex-wrap gap-x-4 gap-y-1 font-mono">
                          {parsedDoc.generates.map((g, i) => (
                            <div
                              key={i}
                              className="flex items-center space-x-1"
                            >
                              <span className="text-red-500 font-bold">»</span>
                              <span>
                                generate{" "}
                                <strong className="text-white">
                                  {g.format}
                                </strong>{" "}
                                &rarr;{" "}
                                <strong className="text-zinc-400">
                                  &quot;{g.path}&quot;
                                </strong>
                              </span>
                            </div>
                          ))}
                        </div>
                      </div>
                    ) : (
                      <div className="bg-[#1f0d0d] border border-red-950/40 p-2.5 rounded-sm text-[11px] text-zinc-500 italic mb-3 shrink-0">
                        Zero generate directives found in DSL files. Add
                        &quot;generate c &quot;out/config.h&quot;;&quot; in your
                        spec editor to link paths.
                      </div>
                    )}

                    {/* Exporter Block view container */}
                    <div className="flex-1 flex flex-col bg-[#050507] border border-zinc-900 rounded p-3 relative min-h-0">
                      <div className="absolute right-3 top-3 flex items-center space-x-2 select-none z-10">
                        {/* Target generator active status indicator */}
                        {(() => {
                          const directive = parsedDoc.generates?.find(
                            (g) => g.format === activeTab,
                          );
                          if (directive) {
                            return (
                              <button
                                onClick={handleSyncTarget}
                                className="bg-[#ff3838] hover:bg-[#ff1e1e] text-white font-extrabold text-[10px] px-3 py-1 border border-black shadow shadow-red-950/20 flex items-center space-x-1"
                              >
                                <span>
                                  ⚡ Synchronize To &quot;{directive.path}&quot;
                                </span>
                              </button>
                            );
                          } else {
                            return (
                              <span className="text-[9px] text-zinc-550 border border-zinc-900 px-2 py-1 bg-black/60 italic font-mono">
                                No DSL Path Tag Defined For {activeTab}
                              </span>
                            );
                          }
                        })()}

                        <button
                          onClick={() =>
                            copyToClipboard(activeCodeContent, activeTab)
                          }
                          className="bg-zinc-900 hover:bg-zinc-800 text-[#e0e0e0] hover:text-white text-[10px] font-extrabold px-3 py-1 border border-zinc-700 transition-all flex items-center space-x-1.5"
                        >
                          {copiedId === activeTab ? (
                            <>
                              <Check className="w-3 h-3 text-emerald-500" />
                              <span>Copied ✓</span>
                            </>
                          ) : (
                            <>
                              <span>Copy</span>
                            </>
                          )}
                        </button>
                      </div>

                      <pre className="flex-1 overflow-auto text-[10.5px] leading-relaxed text-zinc-300 font-mono font-medium p-1 select-text whitespace-pre">
                        {activeCodeContent}
                      </pre>
                    </div>

                    <div className="text-[10px] text-zinc-500 pt-2 flex items-center justify-between shrink-0 font-bold uppercase">
                      <span>Exporter engine: ZXCodeGen v2.5</span>
                      <span>
                        Fomrmat: .
                        {activeTab === "makefile"
                          ? "mk"
                          : activeTab === "env"
                            ? "env"
                            : activeTab}
                      </span>
                    </div>
                  </div>
                )}

                {/* 4. AST EXPLORER DIAGNOSTICS WINDOW INTERNAL */}
                {isAst && (
                  <div className="flex-1 flex flex-col min-h-0">
                    <div className="text-xs text-zinc-400 pb-2 mb-3 border-b border-zinc-800 flex items-center justify-between shrink-0 font-bold uppercase">
                      <span>Syntax Tree Trace & Dependent Relations</span>
                    </div>

                    {/* Quick tracker selection */}
                    <div className="bg-zinc-950 p-3 border border-zinc-900 space-y-2 mb-4 shrink-0">
                      <label className="text-[10px] text-zinc-450 font-bold block uppercase tracking-wide">
                        Click parameter in panel OR select variable here:
                      </label>
                      <select
                        value={dependencyTraceId}
                        onChange={(e) => setDependencyTraceId(e.target.value)}
                        className="w-full font-mono text-xs bg-zinc-900 border border-zinc-800 text-white p-1 focus:outline-none focus:border-white"
                      >
                        <option value="">-- No parameter selected --</option>
                        {Object.keys(parsedDoc.optionsMap).map((id) => (
                          <option key={id} value={id}>
                            {id} ({parsedDoc.optionsMap[id].label})
                          </option>
                        ))}
                      </select>
                    </div>

                    <div className="flex-1 grid grid-rows-2 gap-3 min-h-0">
                      {/* TRACE DEP BOX */}
                      <div className="bg-zinc-950/80 p-3 border border-zinc-900 flex flex-col min-h-0 overflow-auto">
                        <span className="text-[10px] text-yellow-500 font-bold border-b border-zinc-900 pb-1 mb-2 block uppercase tracking-wide">
                          [1] Dependencies (Variables that determine{" "}
                          {dependencyTraceId || "Selection"}'s relevance):
                        </span>
                        <div className="text-[11px] leading-relaxed text-zinc-300 font-mono select-text flex-1">
                          {dependencyTraceId ? (
                            <div className="space-y-1.5">
                              <p className="text-zinc-500 font-bold">
                                Expressions checked:
                              </p>
                              {parsedDoc.optionsMap[dependencyTraceId]
                                ?.visibleWhen && (
                                <p className="text-zinc-300">
                                  - Visibility:{" "}
                                  <code className="text-white bg-zinc-90 w-max px-1 font-mono">
                                    visible_when:{" "}
                                    {parsedDoc.optionsMap[dependencyTraceId]
                                      .visibleWhen?.name || "complex block"}
                                  </code>
                                </p>
                              )}
                              {parsedDoc.optionsMap[dependencyTraceId]
                                ?.constraint && (
                                <p className="text-zinc-300">
                                  - Constraints:{" "}
                                  <code className="text-white bg-zinc-90 w-max px-1 font-mono">
                                    constraint:{" "}
                                    {parsedDoc.optionsMap[dependencyTraceId]
                                      .constraint?.name || "complex block"}
                                  </code>
                                </p>
                              )}
                              <p className="mt-2 text-zinc-400 font-bold">
                                Ultimate resolution required ID:{" "}
                                <span className="text-white underline">
                                  {traceInfo.depString}
                                </span>
                              </p>
                            </div>
                          ) : (
                            <span className="text-zinc-650 italic">
                              Select an option reference key to evaluate.
                            </span>
                          )}
                        </div>
                      </div>

                      {/* TRACE AFFECTEDS BOX */}
                      <div className="bg-zinc-950/80 p-3 border border-zinc-900 flex flex-col min-h-0 overflow-auto">
                        <span className="text-[10px] text-[#ff4f4f] font-bold border-b border-zinc-900 pb-1 mb-2 block uppercase tracking-wide">
                          [2] Dependent Cascading Impacts (Variables affected by
                          toggling {dependencyTraceId || "Selection"}):
                        </span>
                        <div className="text-[11px] leading-relaxed text-zinc-300 font-mono select-text flex-1">
                          {dependencyTraceId ? (
                            <div className="space-y-1.5">
                              <p className="text-zinc-400">
                                If you modify{" "}
                                <span className="text-white font-bold">
                                  {dependencyTraceId}
                                </span>
                                , the following parameters are dynamically
                                re-evaluated in real time:
                              </p>
                              <div className="text-white font-bold border border-zinc-900 p-1 bg-zinc-950 flex flex-wrap gap-1.5 mt-1">
                                {traceDeps.dependents.length > 0 ? (
                                  traceDeps.dependents.map((item) => (
                                    <span
                                      key={item}
                                      className="bg-zinc-900 border border-zinc-800 px-1 py-0.5 text-[9.5px]"
                                    >
                                      {item}
                                    </span>
                                  ))
                                ) : (
                                  <span className="text-zinc-500 italic text-[10px] font-normal">
                                    None. Safe to toggling with zero fallout
                                    cascade risks.
                                  </span>
                                )}
                              </div>
                            </div>
                          ) : (
                            <span className="text-zinc-650 italic">
                              Select an option reference key to evaluate.
                            </span>
                          )}
                        </div>
                      </div>
                    </div>
                  </div>
                )}

                {/* 5. FILESYSTEM HOST LINK & CONSOLE WINDOW INTERNAL */}
                {isFs && (
                  <div className="flex-1 flex flex-col min-h-0 font-mono">
                    {/* Status grid */}
                    <div className="grid grid-cols-2 gap-2 mb-4 shrink-0 text-[11px]">
                      <div className="bg-zinc-950 p-2.5 border border-zinc-900 space-y-1 rounded-sm">
                        <span className="text-zinc-550 block text-[9px] font-bold uppercase">
                          HOST TARGET LINK:
                        </span>
                        <span className="text-emerald-400 font-extrabold flex items-center space-x-1">
                          <span className="w-1.5 h-1.5 rounded-full bg-emerald-500 animate-pulse"></span>
                          <span>ONLINE ACTIVE</span>
                        </span>
                      </div>
                      <div className="bg-zinc-950 p-2.5 border border-zinc-900 space-y-1 rounded-sm">
                        <span className="text-zinc-550 block text-[9px] font-bold uppercase">
                          FILE: /ZXConfig:
                        </span>
                        <span
                          className={
                            filesMap["ZXConfig"]
                              ? "text-white font-extrabold"
                              : "text-yellow-500 font-extrabold"
                          }
                        >
                          {filesMap["ZXConfig"]
                            ? "PRESENT (100% OK)"
                            : "MISSING (BOOT FAILURE)"}
                        </span>
                      </div>
                    </div>

                    <div className="text-xs text-zinc-500 font-bold uppercase mb-2">
                      Workspace Actions Panel:
                    </div>

                    {/* Action buttons list */}
                    <div className="grid grid-cols-2 gap-2 pb-4 border-b border-zinc-800 shrink-0">
                      <button
                        onClick={handleRestoreTemplate}
                        className="bg-white hover:bg-zinc-200 text-black font-extrabold p-2.5 text-[10.5px] border border-black hover:translate-y-[1px] transition-all flex items-center justify-center space-x-1.5 uppercase"
                      >
                        <RotateCcw className="w-3.5 h-3.5 shrink-0" />
                        <span>Force Reset</span>
                      </button>

                      <button
                        onClick={handleDeleteZXConfig}
                        className="bg-[#5c0d0d] hover:bg-[#801414] text-white font-extrabold p-2.5 text-[10.5px] border border-[#ff3838]/80 hover:translate-y-[1px] transition-all flex items-center justify-center space-x-1.5 uppercase"
                      >
                        <Trash2 className="w-3.5 h-3.5 shrink-0" />
                        <span>Delete ZXConfig</span>
                      </button>
                    </div>

                    {/* Console log ticker */}
                    <div className="flex-1 flex flex-col min-h-0 mt-4 bg-black border border-zinc-900 rounded p-2.5 relative">
                      <div className="flex items-center justify-between border-b border-zinc-950 pb-1.5 mb-2 text-[10px] text-zinc-500 font-bold uppercase">
                        <span>Syslog Console</span>
                        <button
                          onClick={() => setServerLogs([])}
                          className="hover:text-white transition-colors"
                        >
                          Clear
                        </button>
                      </div>

                      <div className="flex-1 overflow-y-auto text-[10px] leading-relaxed text-[#a8a8b2] space-y-1 pr-1 font-mono font-medium max-h-[160px] select-text">
                        {serverLogs.length === 0 ? (
                          <div className="text-zinc-650 italic">
                            [Listening on API loop channels...]
                          </div>
                        ) : (
                          serverLogs.map((log, index) => (
                            <div key={index} className="truncate select-text">
                              {log}
                            </div>
                          ))
                        )}
                      </div>
                    </div>
                  </div>
                )}

                {/* 6. ABOUT & KERNEL DUMP WINDOW INTERNAL */}
                {win.id === "about" && (
                  <div className="flex-1 flex flex-col min-h-0 text-[11px] font-mono select-text">
                    <div className="flex items-center justify-between border-b border-[#30363d] pb-2 mb-3 shrink-0">
                      <span className="text-[#ff7b72] font-bold uppercase tracking-wide">
                        [System Diagnostics Report]
                      </span>
                      <button
                        onClick={fetchSysDump}
                        className="bg-[#21262d] hover:bg-[#30363d] text-[#c9d1d9] font-bold text-[9px] px-2.5 py-1 border border-[#30363d] rounded transition-all shrink-0 uppercase"
                      >
                        Re-Poll System state
                      </button>
                    </div>

                    {sysDumpLoading ? (
                      <div className="flex-1 flex items-center justify-center text-zinc-500 italic">
                        <span className="mr-2 h-3.5 w-3.5 animate-spin rounded-full border-2 border-[#8b949e] border-t-white"></span>
                        Analyzing host...
                      </div>
                    ) : (
                      <div className="flex-1 overflow-auto space-y-4 pr-1">
                        {/* Summary Block */}
                        <div className="bg-[#161b22] border border-[#30363d] p-3 rounded-md">
                          <code className="text-[#3fb950] font-bold text-xs block mb-1">
                            » ZXConfig v2.5 copyright (c) 2026 assembler-0
                          </code>
                        </div>

                        {/* Host metrics Group */}
                        <div>
                          <div className="text-[9.5px] text-[#58a6ff] font-bold border-b border-[#30363d] pb-1 mb-2 uppercase select-none tracking-wider font-sans">
                            Target Metrics
                          </div>
                          <div className="grid grid-cols-2 gap-x-4 gap-y-1.5 leading-relaxed">
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Node JS Engine:
                              </span>
                              <span className="text-[#79c0ff] font-bold">
                                {sysDumpData?.nodeVersion || "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Host Architecture:
                              </span>
                              <span className="text-[#f0f6fc]">
                                {sysDumpData?.arch || "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Target Host Platform:
                              </span>
                              <span className="text-[#79c0ff] font-bold">
                                {sysDumpData?.platform || "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Host Release:
                              </span>
                              <span
                                className="text-[#f0f6fc] truncate max-w-[120px]"
                                title={sysDumpData?.release}
                              >
                                {sysDumpData?.release || "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Total Core Memory:
                              </span>
                              <span className="text-[#f0f6fc]">
                                {sysDumpData?.totalMem
                                  ? `${(sysDumpData.totalMem / 1024 / 1024 / 1024).toFixed(2)} GB`
                                  : "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Available Idle Memory:
                              </span>
                              <span className="text-[#3fb950] font-bold">
                                {sysDumpData?.freeMem
                                  ? `${(sysDumpData.freeMem / 1024 / 1024 / 1024).toFixed(2)} GB`
                                  : "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5 col-span-2">
                              <span className="text-[#8b949e] shrink-0">
                                CPU Microarchitecture:
                              </span>
                              <span
                                className="text-[#f0f6fc] text-right font-medium"
                                title={sysDumpData?.cpuModel}
                              >
                                {sysDumpData?.cpuModel || "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Max Hardware Threads:
                              </span>
                              <span className="text-[#79c0ff] font-bold">
                                {sysDumpData?.cpuThreads
                                  ? `${sysDumpData.cpuThreads} Threads`
                                  : "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Host Container Uptime:
                              </span>
                              <span className="text-[#f0f6fc]">
                                {sysDumpData?.uptime
                                  ? `${(sysDumpData.uptime / 3600).toFixed(2)} Hrs`
                                  : "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Process Runtime Env:
                              </span>
                              <span className="text-[#ff7b72] font-semibold">
                                {sysDumpData?.env || "production"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Process ID (PID):
                              </span>
                              <span className="text-[#79c0ff] font-mono">
                                {sysDumpData?.pid || "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5 col-span-2">
                              <span className="text-[#8b949e] shrink-0">
                                Active File Workspace Root:
                              </span>
                              <span
                                className="text-[#ff9b72] font-mono text-right truncate break-all block max-w-sm"
                                title={sysDumpData?.cwd}
                              >
                                {sysDumpData?.cwd || "Unknown"}
                              </span>
                            </div>
                          </div>
                        </div>

                        {/* Client metrics Group */}
                        <div>
                          <div className="text-[9.5px] text-[#58a6ff] font-bold border-b border-[#30363d] pb-1 mb-2 uppercase select-none tracking-wider font-sans">
                            Client Environment
                          </div>
                          <div className="grid grid-cols-2 gap-x-4 gap-y-1.5 leading-relaxed">
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Screen Coordinates:
                              </span>
                              <span className="text-[#f0f6fc] font-bold">
                                {window.screen?.width || 0}px x{" "}
                                {window.screen?.height || 0}px
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Inner Frame Dimensions:
                              </span>
                              <span className="text-[#f0f6fc] font-bold">
                                {window.innerWidth || 0}px x{" "}
                                {window.innerHeight || 0}px
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Max Cores Detected:
                              </span>
                              <span className="text-[#79c0ff] font-bold">
                                {navigator.hardwareConcurrency || "Unknown"}{" "}
                                vCPUs
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Locales Language Config:
                              </span>
                              <span className="text-[#f0f6fc] font-bold">
                                {navigator.language || "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Cookie State:
                              </span>
                              <span className="text-[#3fb950] font-bold">
                                {navigator.cookieEnabled
                                  ? "ENABLED"
                                  : "DISABLED"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5">
                              <span className="text-[#8b949e]">
                                Application Platform:
                              </span>
                              <span className="text-[#f0f6fc] font-bold">
                                {navigator.platform || "Unknown"}
                              </span>
                            </div>
                            <div className="flex justify-between border-b border-[#21262d] py-0.5 col-span-2">
                              <span className="text-[#8b949e] shrink-0">
                                Client User Agent String:
                              </span>
                              <span
                                className="text-[#8b949e] font-mono text-right truncate break-all block max-w-sm"
                                title={navigator.userAgent}
                              >
                                {navigator.userAgent || "Unknown"}
                              </span>
                            </div>
                          </div>
                        </div>
                      </div>
                    )}
                  </div>
                )}
              </div>

              {/* Bottom-right diagonal resize gripper */}
              {!win.isMaximized && (
                <div
                  onMouseDown={(e) => handleResizeMouseDown(win.id, e)}
                  className="absolute bottom-1 right-1 w-4 h-4 cursor-se-resize z-[99] flex items-end justify-end p-[1px] select-none group"
                  title="Resize window"
                >
                  <svg
                    width="8"
                    height="8"
                    viewBox="0 0 10 10"
                    className="text-[#8b949e] group-hover:text-[#58a6ff] transition-colors pointer-events-none"
                  >
                    <line
                      x1="1"
                      y1="9"
                      x2="9"
                      y2="1"
                      stroke="currentColor"
                      strokeWidth="1.5"
                      strokeLinecap="round"
                      opacity="0.4"
                    />
                    <line
                      x1="4"
                      y1="9"
                      x2="9"
                      y2="4"
                      stroke="currentColor"
                      strokeWidth="1.5"
                      strokeLinecap="round"
                      opacity="0.7"
                    />
                    <line
                      x1="7"
                      y1="9"
                      x2="9"
                      y2="7"
                      stroke="currentColor"
                      strokeWidth="1.5"
                      strokeLinecap="round"
                    />
                  </svg>
                </div>
              )}
            </div>
          );
        })}
      </div>

      {/* Dynamic Status / Taskbar Footer */}
      <footer className="h-8 bg-black border-t border-zinc-800 px-4 flex items-center justify-between shrink-0 select-none z-[100] text-[11px] text-zinc-400">
        <div className="flex items-center space-x-4">
          <div className="flex items-center space-x-1">
            <span className="text-zinc-550 mr-1">Compiler Sync: </span>
            <span
              className={`${parseErrors.length > 0 ? "text-yellow-500 font-bold" : "text-emerald-500 font-bold"}`}
            >
              {parseErrors.length > 0 ? "Warnings/Errors Detected" : "Verified"}
            </span>
          </div>

          <div className="h-3 border-r border-zinc-800"></div>

          <div className="flex items-center space-x-1.5">
            <span className="text-zinc-504">Constraints check:</span>
            {constraintErrors.length === 0 ? (
              <span className="text-white font-extrabold bg-zinc-900 border border-zinc-800 px-1 text-[9px]">
                Passed
              </span>
            ) : (
              <span className="text-yellow-400 font-extrabold bg-[#2a1b05] border border-yellow-700 px-1 text-[9px] animate-pulse">
                {constraintErrors.length} Failing
              </span>
            )}
          </div>
        </div>

        <div className="flex items-center space-x-2">
          <span className="text-zinc-550">
            Double-click title bar to toggle expand // Grab black frames to drag
          </span>
        </div>
      </footer>
    </div>
  );
}
