/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

export type OptionType = "bool" | "int" | "string" | "select";

export interface Expression {
  type: "binary" | "unary" | "literal" | "variable";
  operator?: string;
  left?: Expression;
  right?: Expression;
  value?: string | number | boolean;
  name?: string; // variable name
}

export interface OptionNode {
  id: string;
  type: OptionType;
  label: string;
  description?: string;
  defaultValue: string | number | boolean;
  range?: [number, number]; // [min, max] for int
  values?: string[]; // for select/enum
  visibleWhen?: Expression;
  constraint?: Expression;
}

export interface GroupNode {
  id: string;
  label: string;
  visibleWhen?: Expression;
  children: OptionNode[];
}

export interface GenerateDirective {
  format: "c" | "rust" | "makefile" | "env";
  path: string;
}

export interface ZXDocument {
  groups: GroupNode[];
  optionsMap: Record<string, OptionNode>;
  generates?: GenerateDirective[];
  includes?: string[];
}

export interface ParseError {
  line: number;
  column: number;
  message: string;
  severity: "error" | "warning";
}

export interface WindowInstance {
  id: string;
  title: string;
  x: number;
  y: number;
  width: number;
  height: number;
  zIndex: number;
  isOpen: boolean;
  isMinimized: boolean;
  isMaximized: boolean;
}

export type ConfigValuesMap = Record<string, string | number | boolean>;
