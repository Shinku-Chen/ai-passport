#!/usr/bin/env python3
"""Parse the source project's branch configuration (转换工具/branchConfig.js).

The MiBand port flattens the whole game into one linear page table and keeps every
"deviation from linear" in this file:

  noNextPages  前进特判:page -> target(走到这一页的下一句时按目标跳,跳过兄弟分支)
  noBackPages  回退堵死:page -> target(往回翻时落到的页;多数是自环)
  hiddenPages  条件路由:page -> (choice) => target(按选项历史决定去哪)
  end          结局页:page -> 结局名

The hiddenPages values are JavaScript closures. Rather than reimplementing the
expressions on the device, we evaluate them here for **every combination of the
choices they depend on** and ship the resulting decision table. That keeps the
firmware free of an expression evaluator and makes the translation verifiable
(the JS subset parser below is covered by tests/test_dracu_branch.py).

Grammar actually used by the file (checked over all 41 entries):

  statement := "const" NAME "=" expr | "if" "(" expr ")" "return" NUMBER | "return" NUMBER
  expr      := or
  or        := and ("||" and)*
  and       := unary ("&&" unary)*
  unary     := "!" unary | primary
  primary   := "(" expr ")" | NAME | "choice" "[" NUMBER "]" ("===" | "==") NUMBER
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# 表达式 AST
# --------------------------------------------------------------------------


class Expr:
    def eval(self, choice: dict[int, int]) -> bool:  # pragma: no cover - 抽象基类
        raise NotImplementedError


@dataclass(frozen=True)
class ChoiceIs(Expr):
    """choice[page] === value(未选时 choice[page] 不存在,恒为 False)。"""

    page: int
    value: int

    def eval(self, choice: dict[int, int]) -> bool:
        return choice.get(self.page, 0) == self.value

    def pages(self) -> set[int]:
        return {self.page}


@dataclass(frozen=True)
class Not(Expr):
    inner: Expr

    def eval(self, choice: dict[int, int]) -> bool:
        return not self.inner.eval(choice)

    def pages(self) -> set[int]:
        return self.inner.pages()


@dataclass(frozen=True)
class And(Expr):
    parts: tuple[Expr, ...]

    def eval(self, choice: dict[int, int]) -> bool:
        return all(part.eval(choice) for part in self.parts)

    def pages(self) -> set[int]:
        result: set[int] = set()
        for part in self.parts:
            result |= part.pages()
        return result


@dataclass(frozen=True)
class Or(Expr):
    parts: tuple[Expr, ...]

    def eval(self, choice: dict[int, int]) -> bool:
        return any(part.eval(choice) for part in self.parts)

    def pages(self) -> set[int]:
        result: set[int] = set()
        for part in self.parts:
            result |= part.pages()
        return result


# --------------------------------------------------------------------------
# 词法 / 语法
# --------------------------------------------------------------------------

TOKEN_RE = re.compile(
    r"\s*(?:(?P<number>\d+)|(?P<name>[A-Za-z_]\w*)|(?P<op>===|==|&&|\|\||[()\[\]!]))"
)


class ParseError(ValueError):
    pass


def tokenize(text: str) -> list[str]:
    tokens: list[str] = []
    position = 0
    while position < len(text):
        match = TOKEN_RE.match(text, position)
        if match is None:
            rest = text[position:].strip()
            if not rest:
                break
            raise ParseError(f"无法解析: {text[position:position + 24]!r}")
        position = match.end()
        tokens.append(match.group("number") or match.group("name") or match.group("op"))
    return tokens


@dataclass
class Parser:
    tokens: list[str]
    env: dict[str, Expr] | None = None
    position: int = 0

    def peek(self) -> str | None:
        return self.tokens[self.position] if self.position < len(self.tokens) else None

    def take(self, expected: str | None = None) -> str:
        token = self.peek()
        if token is None:
            raise ParseError("表达式意外结束")
        if expected is not None and token != expected:
            raise ParseError(f"期望 {expected!r},实际 {token!r}")
        self.position += 1
        return token

    def parse_expr(self) -> Expr:
        return self.parse_or()

    def parse_or(self) -> Expr:
        parts = [self.parse_and()]
        while self.peek() == "||":
            self.take()
            parts.append(self.parse_and())
        return parts[0] if len(parts) == 1 else Or(tuple(parts))

    def parse_and(self) -> Expr:
        parts = [self.parse_unary()]
        while self.peek() == "&&":
            self.take()
            parts.append(self.parse_unary())
        return parts[0] if len(parts) == 1 else And(tuple(parts))

    def parse_unary(self) -> Expr:
        if self.peek() == "!":
            self.take()
            return Not(self.parse_unary())
        return self.parse_primary()

    def parse_primary(self) -> Expr:
        token = self.peek()
        if token == "(":
            self.take()
            inner = self.parse_expr()
            self.take(")")
            return inner
        if token == "choice":
            self.take()
            self.take("[")
            page = int(self.take())
            self.take("]")
            operator = self.take()
            if operator not in ("===", "=="):
                raise ParseError(f"不支持的比较符 {operator!r}")
            value = int(self.take())
            return ChoiceIs(page, value)
        if token is not None and re.fullmatch(r"[A-Za-z_]\w*", token):
            # const 变量(只引用 choice[...],定义在前)
            if not self.env or token not in self.env:
                raise ParseError(f"未定义的变量 {token!r}")
            self.take()
            return self.env[token]
        raise ParseError(f"无法解析的原子 {token!r}")


def parse_expression(text: str, env: dict[str, Expr] | None = None) -> Expr:
    parser = Parser(tokenize(text), env=env)
    expr = parser.parse_expr()
    if parser.peek() is not None:
        raise ParseError(f"表达式尾部残留: {parser.peek()!r}")
    return expr


# --------------------------------------------------------------------------
# hiddenPages 闭包 → 决策表
# --------------------------------------------------------------------------


@dataclass
class HiddenRule:
    condition: Expr
    target: int


@dataclass(frozen=True)
class Literal:
    """一个正向条件:choice[page] === value。"""

    page: int
    value: int


@dataclass
class HiddenPage:
    page: int
    rules: list[HiddenRule] = field(default_factory=list)
    fallback: int = 0

    def evaluate(self, choice: dict[int, int]) -> int:
        for rule in self.rules:
            if rule.condition.eval(choice):
                return rule.target
        return self.fallback

    def pages(self) -> list[int]:
        result: set[int] = set()
        for rule in self.rules:
            result |= rule.condition.pages()
        return sorted(result)


def parse_hidden_body(page: int, body: str) -> HiddenPage:
    env: dict[str, Expr] = {}
    hidden = HiddenPage(page=page)
    for statement in body.split(";"):
        statement = statement.strip()
        if not statement:
            continue
        const_match = re.match(r"^const\s+([A-Za-z_]\w*)\s*=\s*(.+)$", statement, re.S)
        if const_match:
            env[const_match.group(1)] = parse_expression(const_match.group(2).strip(), env)
            continue
        if_match = re.match(r"^if\s*\((.+)\)\s*return\s+(\d+)$", statement, re.S)
        if if_match:
            hidden.rules.append(HiddenRule(parse_expression(if_match.group(1).strip(), env),
                                           int(if_match.group(2))))
            continue
        return_match = re.match(r"^return\s+(\d+)$", statement)
        if return_match:
            hidden.fallback = int(return_match.group(1))
            continue
        raise ParseError(f"第 {page} 页的条件体无法解析: {statement!r}")
    if hidden.fallback == 0:
        raise ParseError(f"第 {page} 页的条件体没有默认 return")
    return hidden


# --------------------------------------------------------------------------
# 顶层解析
# --------------------------------------------------------------------------

BLOCK_RE = re.compile(r"^\s{2}(\w+):\s*\{", re.M)
PAIR_RE = re.compile(r'"(\d+)"\s*:\s*"([^"]*)"')
HIDDEN_RE = re.compile(r"^\s*(\d+):\s*\(choice\)\s*=>\s*\{(.*)\},?\s*(?://.*)?$", re.M)


def _block(text: str, name: str) -> str:
    """取出 "<name>: { ... }" 的块体(按第一层缩进闭合大括号结束)。"""
    start = re.search(rf"^\s{{2}}{name}:\s*\{{", text, re.M)
    if start is None:
        return ""
    depth = 0
    position = start.end() - 1
    begin = position + 1
    while position < len(text):
        char = text[position]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[begin:position]
        position += 1
    raise ParseError(f"{name} 块没有闭合")


def parse_branch_config(text: str) -> dict:
    """返回 {no_next, no_back, end, hidden}:
      no_next / no_back : {页号: 目标页号}
      end               : {页号: 结局名}
      hidden            : {页号: HiddenPage}
    """
    result: dict = {"no_next": {}, "no_back": {}, "end": {}, "hidden": {}}
    for key, target in (("noNextPages", "no_next"), ("noBackPages", "no_back"), ("end", "end")):
        for page, value in PAIR_RE.findall(_block(text, key)):
            result[target][int(page)] = value if target == "end" else int(value)
    for match in HIDDEN_RE.finditer(_block(text, "hiddenPages")):
        page = int(match.group(1))
        result["hidden"][page] = parse_hidden_body(page, match.group(2))
    return result


def normalise(hidden: HiddenPage) -> HiddenPage:
    """把唯一的取反形式消掉,让所有条件都只剩正向字面量。

    源文件里只有一处取反:`if(!(m||a||r||e||n)) return T1; return T2;`。
    按 if 链语义它等价于「C 成立走 T2,否则 T1」,直接交换即可 —— 这样就不用
    把取反展开成 DNF(那会变成 4^5 个子句)。
    """
    if not hidden.rules:
        return hidden
    last = hidden.rules[-1]
    if isinstance(last.condition, Not):
        if len(hidden.rules) != 1:
            raise ParseError(f"第 {hidden.page} 页的取反不在链尾,无法归一化")
        return HiddenPage(
            page=hidden.page,
            rules=[HiddenRule(last.condition.inner, hidden.fallback)],
            fallback=last.target,
        )
    return hidden


def dnf_clauses(expr: Expr) -> list[tuple[Literal, ...]]:
    """正向条件的 DNF:每个子句是一组必须同时成立的 choice[page] === value。"""
    if isinstance(expr, ChoiceIs):
        return [(Literal(expr.page, expr.value),)]
    if isinstance(expr, Or):
        clauses: list[tuple[Literal, ...]] = []
        for part in expr.parts:
            clauses.extend(dnf_clauses(part))
        return clauses
    if isinstance(expr, And):
        clauses = [()]
        for part in expr.parts:
            merged: list[tuple[Literal, ...]] = []
            for left in clauses:
                for right in dnf_clauses(part):
                    merged.append(left + right)
            clauses = merged
        return clauses
    raise ParseError("取反无法直接展开为正向子句")


def compile_hidden(hidden: HiddenPage, option_counts: dict[int, int]) -> dict:
    """把条件路由编成设备侧要的「规则表 + 默认目标」。

    返回 {"rules": [(子句, 目标)...], "fallback": 目标, "pages": [选项页...]}:
    子句是一组 (cond_index, value),按顺序匹配,全部不中走 fallback。
    cond_index 是条件里出现过的选项页在 pages 里的下标(见 build_cond_pages)。
    """
    normalised = normalise(hidden)
    pages = normalised.pages()
    for page in pages:
        if page not in option_counts:
            raise ParseError(f"条件路由引用了没有选项的页 {page}")
    rules: list[tuple[tuple[Literal, ...], int]] = []
    for rule in normalised.rules:
        for clause in dnf_clauses(rule.condition):
            rules.append((clause, rule.target))
    return {"rules": rules, "fallback": normalised.fallback, "pages": pages}


def verify_hidden(hidden: HiddenPage, compiled: dict, option_counts: dict[int, int]) -> None:
    """穷举依赖选项的全部组合,校核「规则表」与原始 if 链结果完全一致。"""
    pages = compiled["pages"]
    for page in pages:
        if page not in option_counts:
            raise ParseError(f"条件路由引用了没有选项的页 {page}")
    # 每个选项页取 0(未选)或 1..N
    ranges = [range(option_counts[page] + 1) for page in pages]
    for combination in _product(ranges):
        choice = {page: value for page, value in zip(pages, combination)}
        expected = hidden.evaluate(choice)
        index = {page: slot for slot, page in enumerate(pages)}
        actual = compiled["fallback"]
        for clause, target in compiled["rules"]:
            if all(choice[literal.page] == literal.value for literal in clause):
                actual = target
                break
        if actual != expected:
            raise AssertionError(
                f"第 {hidden.page} 页在 {choice} 上不一致: 原始 {expected} vs 规则表 {actual}"
            )


def _product(ranges):
    if not ranges:
        yield ()
        return
    head, *tail = ranges
    for value in head:
        for rest in _product(tail):
            yield (value,) + rest


def build_cond_pages(hidden_pages: dict[int, HiddenPage]) -> list[int]:
    """所有条件里引用到的选项页(升序):设备侧的选择历史就按这个下标存。"""
    result: set[int] = set()
    for hidden in hidden_pages.values():
        result |= set(hidden.pages())
    return sorted(result)


def option_counts(pages: dict[int, dict]) -> dict[int, int]:
    """每个选项页有几个选项(c1..c5 里非空的个数)。"""
    counts: dict[int, int] = {}
    for page, record in pages.items():
        if not record.get("co"):
            continue
        counts[page] = sum(1 for index in range(1, 6) if str(record.get(f"c{index}") or "").strip())
    return counts
