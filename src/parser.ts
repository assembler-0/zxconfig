/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

import {
  OptionType,
  Expression,
  OptionNode,
  GroupNode,
  ZXDocument,
  ParseError,
  ConfigValuesMap
} from "./types";

export interface Token {
  type:
    | "KEYWORD"
    | "IDENTIFIER"
    | "STRING"
    | "NUMBER"
    | "BOOLEAN"
    | "OPERATOR"
    | "PUNCTUATION"
    | "UNKNOWN";
  value: string;
  line: number;
  column: number;
}

// Helper to determine if character is whitespace
const isWhitespace = (ch: string) => /\s/.test(ch);

// Helper to check if identifier character
const isIdentChar = (ch: string) => /[a-zA-Z0-9_]/.test(ch);

export function tokenize(source: string): Token[] {
  const tokens: Token[] = [];
  let i = 0;
  let line = 1;
  let column = 1;

  while (i < source.length) {
    let ch = source[i];

    // Handle comments (both // and #)
    if (ch === "#" || (ch === "/" && source[i + 1] === "/")) {
      while (i < source.length && source[i] !== "\n") {
        i++;
        column++;
      }
      continue;
    }

    if (ch === "\n") {
      line++;
      column = 1;
      i++;
      continue;
    }

    if (isWhitespace(ch)) {
      column++;
      i++;
      continue;
    }

    // Punctuations: { } [ ] : ; , ( )
    if ("{}[]:;,()".indexOf(ch) !== -1) {
      tokens.push({
        type: "PUNCTUATION",
        value: ch,
        line,
        column,
      });
      column++;
      i++;
      continue;
    }

    // Strings declared with double quotes
    if (ch === '"') {
      let strVal = "";
      const startCol = column;
      i++; // skip open quote
      column++;

      while (i < source.length && source[i] !== '"') {
        if (source[i] === "\n") {
          line++;
          column = 1;
        } else {
          column++;
        }
        strVal += source[i];
        i++;
      }

      if (i < source.length && source[i] === '"') {
        i++; // skip close quote
        column++;
      }

      tokens.push({
        type: "STRING",
        value: strVal,
        line,
        column: startCol,
      });
      continue;
    }

    // Operators: &&, ||, ==, !=, <=, >=, <, >, !
    if (
      ch === "&" && source[i + 1] === "&" ||
      ch === "|" && source[i + 1] === "|" ||
      ch === "=" && source[i + 1] === "=" ||
      ch === "!" && source[i + 1] === "=" ||
      ch === "<" && source[i + 1] === "=" ||
      ch === ">" && source[i + 1] === "="
    ) {
      const op = source.slice(i, i + 2);
      tokens.push({
        type: "OPERATOR",
        value: op,
        line,
        column,
      });
      i += 2;
      column += 2;
      continue;
    }

    if ("!<>".indexOf(ch) !== -1) {
      tokens.push({
        type: "OPERATOR",
        value: ch,
        line,
        column,
      });
      i++;
      column++;
      continue;
    }

    // Numbers (integers)
    if (/[0-9]/.test(ch)) {
      let numStr = "";
      const startCol = column;
      while (i < source.length && /[0-9]/.test(source[i])) {
        numStr += source[i];
        i++;
        column++;
      }
      tokens.push({
        type: "NUMBER",
        value: numStr,
        line,
        column: startCol,
      });
      continue;
    }

    // Identifiers and Key Words
    if (/[a-zA-Z_]/.test(ch)) {
      let idStr = "";
      const startCol = column;
      while (i < source.length && isIdentChar(source[i])) {
        idStr += source[i];
        i++;
        column++;
      }

      const keywords = [
        "feature", "option", "group", "type", "label", "description",
        "default", "range", "values", "visible_when", "constraint",
        "bool", "int", "string", "select", "include", "generate"
      ];

      if (keywords.includes(idStr)) {
        tokens.push({
          type: "KEYWORD",
          value: idStr,
          line,
          column: startCol,
        });
      } else if (idStr === "true" || idStr === "false") {
        tokens.push({
          type: "BOOLEAN",
          value: idStr,
          line,
          column: startCol,
        });
      } else {
        tokens.push({
          type: "IDENTIFIER",
          value: idStr,
          line,
          column: startCol,
        });
      }
      continue;
    }

    // Capture single unknown character
    tokens.push({
      type: "UNKNOWN",
      value: ch,
      line,
      column,
    });
    i++;
    column++;
  }

  return tokens;
}

export class Parser {
  private tokens: Token[];
  private current = 0;
  public errors: ParseError[] = [];
  private allFiles: Record<string, string>;
  private processedFiles: Set<string>;
  public includes: string[] = [];
  public generates: { format: "c" | "rust" | "makefile" | "env"; path: string }[] = [];

  constructor(
    tokens: Token[],
    allFiles: Record<string, string> = {},
    processedFiles: Set<string> = new Set()
  ) {
    this.tokens = tokens;
    this.allFiles = allFiles;
    this.processedFiles = processedFiles;
  }

  private isAtEnd() {
    return this.current >= this.tokens.length;
  }

  private peek(): Token {
    if (this.isAtEnd()) {
      return {
        type: "UNKNOWN",
        value: "EOF",
        line: this.tokens[this.tokens.length - 1]?.line || 1,
        column: this.tokens[this.tokens.length - 1]?.column || 1,
      };
    }
    return this.tokens[this.current];
  }

  private previous(): Token {
    return this.tokens[this.current - 1];
  }

  private advance(): Token {
    if (!this.isAtEnd()) this.current++;
    return this.previous();
  }

  private check(type: string, value?: string): boolean {
    if (this.isAtEnd()) return false;
    const token = this.peek();
    if (token.type !== type) return false;
    if (value !== undefined && token.value !== value) return false;
    return true;
  }

  private match(type: string, value?: string): boolean {
    if (this.check(type, value)) {
      this.advance();
      return true;
    }
    return false;
  }

  private consume(type: string, message: string, value?: string): Token {
    if (this.check(type, value)) {
      return this.advance();
    }
    const token = this.peek();
    this.error(token, message);
    throw new Error(message);
  }

  private error(token: Token, message: string) {
    this.errors.push({
      line: token.line,
      column: token.column,
      message: `${message} (found '${token.value}')`,
      severity: "error",
    });
  }

  // Synchronize parser to next declaration block on error
  private synchronize() {
    this.advance();
    while (!this.isAtEnd()) {
      if (this.previous().type === "PUNCTUATION" && this.previous().value === ";") return;
      if (this.previous().type === "PUNCTUATION" && this.previous().value === "}") return;

      const peekVal = this.peek().value;
      if (
        this.peek().type === "KEYWORD" &&
        (peekVal === "feature" || peekVal === "option" || peekVal === "group")
      ) {
        return;
      }
      this.advance();
    }
  }

  public parse(): ZXDocument {
    const groups: GroupNode[] = [];
    const optionsMap: Record<string, OptionNode> = {};

    // Keep flat list of top-level options in a default group or create on the fly
    const globalOptions: OptionNode[] = [];

    while (!this.isAtEnd()) {
      try {
        if (this.match("KEYWORD", "include")) {
          const pathToken = this.consume("STRING", "Expected file path string literal for include directive");
          this.consume("PUNCTUATION", "Expected ';' to terminate include", ";");
          
          const filename = pathToken.value;
          if (this.processedFiles.has(filename)) {
            continue; // Skip already parsed or circular dependent files
          }
          this.processedFiles.add(filename);

          const subContent = this.allFiles[filename];
          if (subContent === undefined) {
            this.errors.push({
              line: pathToken.line,
              column: pathToken.column,
              message: `Inclusion source resolution failure: target file '${filename}' does not exist on disk/file explorer tree.`,
              severity: "error",
            });
          } else {
            const parsedSub = parseZXDSL(subContent, this.allFiles, this.processedFiles);
            this.errors.push(...parsedSub.errors);

            // Merge parsed children options and groups
            for (const key of Object.keys(parsedSub.doc.optionsMap)) {
              optionsMap[key] = parsedSub.doc.optionsMap[key];
            }

            for (const subGroup of parsedSub.doc.groups) {
              const existingGroup = groups.find((g) => g.id === subGroup.id);
              if (existingGroup) {
                existingGroup.children.push(...subGroup.children);
              } else {
                groups.push(subGroup);
              }
            }

            if (parsedSub.doc.generates) {
              this.generates.push(...parsedSub.doc.generates);
            }

            this.includes.push(filename);
            if (parsedSub.doc.includes) {
              this.includes.push(...parsedSub.doc.includes);
            }
          }
        } else if (this.match("KEYWORD", "generate")) {
          const formatToken = this.advance();
          let format: "c" | "rust" | "makefile" | "env";

          const formatVal = formatToken.value;
          if (formatToken.type === "IDENTIFIER" || formatToken.type === "KEYWORD") {
            if (["c", "rust", "makefile", "env"].includes(formatVal)) {
              format = formatVal as any;
            } else {
              this.error(formatToken, `Unsupported target format generator: '${formatVal}'. Use c, rust, makefile, or env`);
              throw new Error("Invalid target format generator");
            }
          } else {
            this.error(formatToken, "Expected generator target format (c, rust, makefile, env) after 'generate' key.");
            throw new Error("Expected generator target format");
          }

          const pathToken = this.consume("STRING", "Expected file path string literal for the output path target");
          this.consume("PUNCTUATION", "Expected ';' to terminate generate instruction line.", ";");

          this.generates.push({ format, path: pathToken.value });
        } else if (this.match("KEYWORD", "feature")) {
          // Top level explicit option declaration
          const node = this.parseOption("feature");
          globalOptions.push(node);
          optionsMap[node.id] = node;
        } else if (this.match("KEYWORD", "group")) {
          // Scoped group declaration
          const group = this.parseGroup(optionsMap);
          const existingGroup = groups.find((g) => g.id === group.id);
          if (existingGroup) {
            existingGroup.children.push(...group.children);
          } else {
            groups.push(group);
          }
        } else {
          const t = this.advance();
          this.error(t, "Unexpected token at top level. Expected 'include', 'generate', 'feature' or 'group'.");
          this.synchronize();
        }
      } catch (err) {
        this.synchronize();
      }
    }

    // If we have some global/top-level features, add them either to a Core group or top-level group
    if (globalOptions.length > 0) {
      const existingGlobal = groups.find((g) => g.id === "global");
      if (existingGlobal) {
        existingGlobal.children.push(...globalOptions);
      } else {
        groups.unshift({
          id: "global",
          label: "Primary Parameters",
          children: globalOptions,
        });
      }
    }

    return {
      groups,
      optionsMap,
      generates: this.generates,
      includes: this.includes,
    };
  }

  private parseOption(kind: "feature" | "option"): OptionNode {
    const idToken = this.consume("IDENTIFIER", `Expected identification name for ${kind}`);
    const id = idToken.value;

    this.consume("PUNCTUATION", "Expected '{' after identifier", "{");

    let type: OptionType = "bool";
    let label = id;
    let description: string | undefined;
    let defaultValue: string | number | boolean = false;
    let range: [number, number] | undefined;
    let values: string[] | undefined;
    let visibleWhen: Expression | undefined;
    let constraint: Expression | undefined;

    while (!this.check("PUNCTUATION", "}")) {
      if (this.isAtEnd()) {
        const errorToken = this.peek();
        this.errors.push({
          line: errorToken.line,
          column: errorToken.column,
          message: `Unclosed option declaration block starting at line ${idToken.line}`,
          severity: "error",
        });
        throw new Error("EOF in option block");
      }

      const keyToken = this.consume("KEYWORD", "Expected config attribute directive inside block");
      const key = keyToken.value;

      this.consume("PUNCTUATION", `Expected ':' after attribute '${key}'`, ":");

      if (key === "type") {
        const typeToken = this.advance();
        if (
          typeToken.type === "KEYWORD" &&
          ["bool", "int", "string", "select"].includes(typeToken.value)
        ) {
          type = typeToken.value as OptionType;
          if (type === "int") defaultValue = 0;
          if (type === "string") defaultValue = "";
          if (type === "select") defaultValue = "";
        } else {
          this.error(typeToken, "Invalid option type format. Allowed types: bool, int, string, select");
        }
      } else if (key === "label") {
        const labelToken = this.consume("STRING", "Expected string literal for option label");
        label = labelToken.value;
      } else if (key === "description") {
        const descToken = this.consume("STRING", "Expected string literal for option description");
        description = descToken.value;
      } else if (key === "default") {
        if (this.match("BOOLEAN")) {
          defaultValue = this.previous().value === "true";
        } else if (this.match("NUMBER")) {
          defaultValue = parseInt(this.previous().value, 10);
        } else if (this.match("STRING")) {
          defaultValue = this.previous().value;
        } else {
          const badToken = this.advance();
          this.error(badToken, "Invalid option default assignment value");
        }
      } else if (key === "range") {
        this.consume("PUNCTUATION", "Expected '[' to define number range bounds", "[");
        const lowerToken = this.consume("NUMBER", "Expected range lower limit integer number");
        this.consume("PUNCTUATION", "Expected ',' to separate range limits", ",");
        const upperToken = this.consume("NUMBER", "Expected range upper limit integer number");
        this.consume("PUNCTUATION", "Expected ']' to end range boundary block", "]");
        range = [parseInt(lowerToken.value, 10), parseInt(upperToken.value, 10)];
      } else if (key === "values") {
        this.consume("PUNCTUATION", "Expected '[' to open pickable custom list values", "[");
        const list: string[] = [];
        if (!this.check("PUNCTUATION", "]")) {
          const itemToken = this.consume("STRING", "Expected string literal in select values list");
          list.push(itemToken.value);
          while (this.match("PUNCTUATION", ",")) {
            const nextItem = this.consume("STRING", "Expected string literal after comma");
            list.push(nextItem.value);
          }
        }
        this.consume("PUNCTUATION", "Expected ']' to close select values list", "]");
        values = list;
      } else if (key === "visible_when") {
        visibleWhen = this.parseExpression();
      } else if (key === "constraint") {
        constraint = this.parseExpression();
      } else {
        this.error(keyToken, `Unsupported config parameter attribute: '${key}'`);
      }

      this.consume("PUNCTUATION", "Expected ';' to close declaration line", ";");
    }

    this.consume("PUNCTUATION", "Expected closing block curly brace '}'", "}");

    // Validate type compatibility
    if (type === "select" && (!values || values.length === 0)) {
      this.errors.push({
        line: idToken.line,
        column: idToken.column,
        message: `Option '${id}' is defined as type 'select' but specifies no active checkable 'values: [...]'`,
        severity: "warning",
      });
    }

    return {
      id,
      type,
      label,
      description,
      defaultValue,
      range,
      values,
      visibleWhen,
      constraint,
    };
  }

  private parseGroup(optionsMap: Record<string, OptionNode>): GroupNode {
    const idToken = this.consume("IDENTIFIER", "Expected reference identifier for group");
    const id = idToken.value;

    this.consume("PUNCTUATION", "Expected '{' to start group declaration scope", "{");

    let label = id;
    let visibleWhen: Expression | undefined;
    const children: OptionNode[] = [];

    while (!this.check("PUNCTUATION", "}")) {
      if (this.isAtEnd()) {
        const errorToken = this.peek();
        this.errors.push({
          line: errorToken.line,
          column: errorToken.column,
          message: `Unclosed group scope block starting at line ${idToken.line}`,
          severity: "error",
        });
        throw new Error("EOF in group block");
      }

      if (this.match("KEYWORD", "label")) {
        this.consume("PUNCTUATION", "Expected ':' after label attribute in group", ":");
        const labelToken = this.consume("STRING", "Expected string literal description description for group");
        label = labelToken.value;
        this.consume("PUNCTUATION", "Expected ';'", ";");
      } else if (this.match("KEYWORD", "visible_when")) {
        this.consume("PUNCTUATION", "Expected ':' after visible_when attribute in group", ":");
        visibleWhen = this.parseExpression();
        this.consume("PUNCTUATION", "Expected ';'", ";");
      } else if (this.match("KEYWORD", "option")) {
        try {
          const optNode = this.parseOption("option");
          children.push(optNode);
          optionsMap[optNode.id] = optNode;
        } catch (e) {
          this.synchronize();
        }
      } else {
        const t = this.advance();
        this.error(t, "Unexpected attribute or subnode in group. Expected 'label', 'visible_when', or 'option'.");
        this.synchronize();
      }
    }

    this.consume("PUNCTUATION", "Expected closing block curly brace '}'", "}");

    return {
      id,
      label,
      visibleWhen,
      children,
    };
  }

  // Recursive expression parsing
  private parseExpression(): Expression {
    return this.parseLogicalOr();
  }

  private parseLogicalOr(): Expression {
    let expr = this.parseLogicalAnd();
    while (this.match("OPERATOR", "||")) {
      const right = this.parseLogicalAnd();
      expr = {
        type: "binary",
        operator: "||",
        left: expr,
        right,
      };
    }
    return expr;
  }

  private parseLogicalAnd(): Expression {
    let expr = this.parseEquality();
    while (this.match("OPERATOR", "&&")) {
      const right = this.parseEquality();
      expr = {
        type: "binary",
        operator: "&&",
        left: expr,
        right,
      };
    }
    return expr;
  }

  private parseEquality(): Expression {
    let expr = this.parseComparison();
    while (this.match("OPERATOR", "==") || this.match("OPERATOR", "!=")) {
      const op = this.previous().value;
      const right = this.parseComparison();
      expr = {
        type: "binary",
        operator: op,
        left: expr,
        right,
      };
    }
    return expr;
  }

  private parseComparison(): Expression {
    let expr = this.parseUnary();
    while (
      this.match("OPERATOR", "<") ||
      this.match("OPERATOR", ">") ||
      this.match("OPERATOR", "<=") ||
      this.match("OPERATOR", ">=")
    ) {
      const op = this.previous().value;
      const right = this.parseUnary();
      expr = {
        type: "binary",
        operator: op,
        left: expr,
        right,
      };
    }
    return expr;
  }

  private parseUnary(): Expression {
    if (this.match("OPERATOR", "!")) {
      const inner = this.parseUnary();
      return {
        type: "unary",
        operator: "!",
        left: inner,
      };
    }
    return this.parsePrimary();
  }

  private parsePrimary(): Expression {
    if (this.match("BOOLEAN")) {
      return { type: "literal", value: this.previous().value === "true" };
    }
    if (this.match("NUMBER")) {
      return { type: "literal", value: parseInt(this.previous().value, 10) };
    }
    if (this.match("STRING")) {
      return { type: "literal", value: this.previous().value };
    }
    if (this.match("IDENTIFIER")) {
      return { type: "variable", name: this.previous().value };
    }
    if (this.match("PUNCTUATION", "(")) {
      const expr = this.parseExpression();
      this.consume("PUNCTUATION", "Expected closing bracket ')' to couple nesting", ")");
      return expr;
    }

    const t = this.peek();
    this.error(t, "Malformed syntax expression subsegment block structure");
    throw new Error("Malformed expression");
  }
}

// Global Parser entrypoint
export function parseZXDSL(
  source: string,
  allFiles: Record<string, string> = {},
  processedFiles: Set<string> = new Set()
): { doc: ZXDocument; errors: ParseError[] } {
  try {
    const tokens = tokenize(source);
    const parser = new Parser(tokens, allFiles, processedFiles);
    const doc = parser.parse();
    return { doc, errors: parser.errors };
  } catch (err: any) {
    return {
      doc: { groups: [], optionsMap: {}, generates: [], includes: [] },
      errors: [
        {
          line: 1,
          column: 1,
          message: `Fatal Lexical Parser Interruption: ${err.message}`,
          severity: "error",
        },
      ],
    };
  }
}

// AST Evaluator Engine
export function evaluateExpression(
  expr: Expression | undefined,
  values: ConfigValuesMap,
  allOptions: Record<string, OptionNode>
): any {
  if (!expr) return true; // Absence of expression implies always valid/visible

  switch (expr.type) {
    case "literal":
      return expr.value;

    case "variable": {
      const varName = expr.name || "";
      // If variable value isn't mapped, retrieve the default schema value
      if (values[varName] !== undefined) {
        return values[varName];
      }
      const node = allOptions[varName];
      if (node) {
        return node.defaultValue;
      }
      // If undefined completely, fall back to false/0/empty string
      return false;
    }

    case "unary": {
      const val = evaluateExpression(expr.left, values, allOptions);
      if (expr.operator === "!") {
        return !val;
      }
      return val;
    }

    case "binary": {
      const lhs = evaluateExpression(expr.left, values, allOptions);
      // Short-circuit evaluations for logical operators
      if (expr.operator === "&&") {
        return !!lhs && !!evaluateExpression(expr.right, values, allOptions);
      }
      if (expr.operator === "||") {
        return !!lhs || !!evaluateExpression(expr.right, values, allOptions);
      }

      const rhs = evaluateExpression(expr.right, values, allOptions);
      switch (expr.operator) {
        case "==":
          return lhs === rhs;
        case "!=":
          return lhs !== rhs;
        case "<":
          return lhs < rhs;
        case ">":
          return lhs > rhs;
        case "<=":
          return lhs <= rhs;
        case ">=":
          return lhs >= rhs;
        default:
          return false;
      }
    }
    default:
      return false;
  }
}

// Check if an option node is actually active (its parents are visible, etc.)
export function isNodeVisible(
  nodeId: string,
  doc: ZXDocument,
  values: ConfigValuesMap
): boolean {
  const node = doc.optionsMap[nodeId];
  if (!node) return false;

  // Evaluate the local visible_when rule
  if (node.visibleWhen && !evaluateExpression(node.visibleWhen, values, doc.optionsMap)) {
    return false;
  }

  // Check if this option is nested in any invisible group
  for (const group of doc.groups) {
    const hasChild = group.children.some((c) => c.id === nodeId);
    if (hasChild) {
      if (group.visibleWhen && !evaluateExpression(group.visibleWhen, values, doc.optionsMap)) {
        return false;
      }
      break;
    }
  }

  return true;
}

// Evaluates all constraint check and returns array of active rule issues/errors
export function evaluateConstraints(
  doc: ZXDocument,
  values: ConfigValuesMap
): { valid: boolean; errors: string[] } {
  const errors: string[] = [];

  for (const group of doc.groups) {
    // Check group visibility, if invisible its rules are bypassed
    const groupVisible = !group.visibleWhen || evaluateExpression(group.visibleWhen, values, doc.optionsMap);
    if (!groupVisible) continue;

    for (const opt of group.children) {
      // Check feature/option visibility
      const optVisible = isNodeVisible(opt.id, doc, values);
      if (!optVisible) continue;

      // Validate core options constraint rule itself
      if (opt.constraint) {
        const satisfied = evaluateExpression(opt.constraint, values, doc.optionsMap);
        if (!satisfied) {
          const constraintStr = stringifyExpression(opt.constraint);
          errors.push(`Constraint violation on '${opt.label}' (${opt.id}): requires [${constraintStr}] to hold.`);
        }
      }

      // Check generic ranges for integers
      if (opt.type === "int" && opt.range) {
        const val = values[opt.id] !== undefined ? values[opt.id] : opt.defaultValue;
        const numVal = typeof val === "number" ? val : parseInt(String(val), 10);
        const [min, max] = opt.range;
        if (numVal < min || numVal > max) {
          errors.push(`Range violation on '${opt.label}' (${opt.id}): Value ${numVal} lies outside limits [${min}, ${max}].`);
        }
      }
    }
  }

  return {
    valid: errors.length === 0,
    errors,
  };
}

// Convert parser AST nodes back to ZXDSL code representation
export function stringifyExpression(expr: Expression | undefined): string {
  if (!expr) return "";
  switch (expr.type) {
    case "literal":
      return typeof expr.value === "string" ? `"${expr.value}"` : String(expr.value);
    case "variable":
      return expr.name || "";
    case "unary":
      return `${expr.operator}${stringifyExpression(expr.left)}`;
    case "binary":
      return `(${stringifyExpression(expr.left)} ${expr.operator} ${stringifyExpression(expr.right)})`;
    default:
      return "";
  }
}
