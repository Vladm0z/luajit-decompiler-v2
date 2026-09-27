# LuaJIT Decompiler v2

LuaJIT Decompiler v2 reconstructs Lua source code from LuaJIT bytecode. It is a maintained fork of [Aussiemon's fork](https://github.com/Aussiemon/luajit-decompiler-v2) of the [original project by marsinator358](https://github.com/marsinator358/luajit-decompiler-v2), which replaced the earlier, now defunct Python-based decompiler.

The tool parses LuaJIT bytecode prototypes, rebuilds control flow and expression structure, and emits formatted Lua source. It supports stripped bytecode (without debug information), `goto` statements, locals, upvalues, and nested function prototypes.

This fork replaces the upstream boolean expression reconstruction with a control-flow-graph-based algorithm derived from the paper *Decompiling Boolean Expressions from Java Bytecode* (Nanda & Arun-Kumar, ISEC 2016), and adds heuristic identifier reconstruction for stripped bytecode.

## Features

- Complete bytecode parsing: prototypes, constants, table and number constants, upvalues, and debug information (variable names, scopes, line map).
- Reconstruction of local variable scopes, upvalue references, and nested function definitions, including method-style definitions.
- Structured control flow: `if`/`elseif`/`else`, numeric and generic `for`, `while`, `repeat`, and `break`. Residual unstructured jumps are emitted as `goto` with labels.
- Boolean expression reconstruction from conditional jump graphs (see [Algorithm](#boolean-expression-reconstruction-astconditiongraphh)).
- Stripped bytecode support with deterministic fallback naming and heuristic name inference.

## Changes relative to upstream

### Boolean expression reconstruction (`ast/conditionGraph.h`)

Upstream resolves sequences of conditional jumps into `and`/`or`/`not` expressions using a linear, adjacency-based builder (`ast/conditionBuilder.h`). This fork replaces the usage of the upstream linear builder with a new control-flow-graph-based algorithm `ast/conditionGraph.h`:

1. **Graph construction:** Every conditional statement contributes a node with two outgoing edges (jump taken, jump not taken); unconditional jumps contribute single-edge nodes. Terminal nodes represent the true target, the false target, and, for boolean assignments, the end of the assignment. Edges are coloured *true* or *false* according to the semantics of the originating opcode (`ISLT`, `ISGE`, `ISLE`, `ISGT`, `ISEQ*`, `ISNE*`, `IST`, `ISF`, `ISTC`, `ISFC`, `JMP`), including operand-order normalisation for swapped comparisons.
2. **Topological ordering:** A depth-first traversal that ignores terminal nodes produces a topological order of the region. Cyclic regions (loop conditions) are rejected and remain handled by the loop structuring pass.
3. **Monochromatic enforcement:** Following the paper, a node may participate in a boolean expression only if all of its incoming edges share a single colour. Nodes with mixed incoming colours are repaired by *twisting* a conflicting predecessor: negating its condition, exchanging its outgoing edges, and recolouring the affected incoming edges. A per-node twist budget prevents non-terminating alternation on graphs that cannot be made monochromatic; such regions are rejected rather than incorrectly reduced.
4. **Pattern reduction:** The graph is reduced to a single node by repeated application of:
    - *Ternary (diamond) pattern:* A node whose two successors converge on the same pair of terminals is rewritten as `(c0 and c1) or (not c0 and c2)`. 
    - *AND/OR chains:* Identified using the Theorem 2 of the paper. The number of same-coloured incoming edges at the successors of the chain root determines the chain type and the traversal length; the anchor node delimits the chain, and the expression tree is rebuilt from the skip-edge structure of the chain, which fixes operator grouping and parenthesisation.
    - *Degenerate two-node chains:* Reduced directly.
5. **Expression emission:** Atoms are emitted with their opcode-derived comparison; negation is tracked per node. In assignment mode, truthiness tests that feed boolean-constant targets are coerced to boolean results, preserving the corresponding upstream behaviour. In statement mode the polarity of the final node is normalised so that the emitted condition governs the `then` branch. Already reduced sub-expressions are carried through subsequent reductions unchanged.

A region that fails any stage is reported and the containing file is skipped (under `-s`) or raised as an assertion, instead of emitting malformed control flow.

### Heuristic identifier reconstruction

When debug information is absent or ignored (`-i`), upstream emits positional names only (`var_F_N`, `arg_F_N`, `iter_F_N`). This fork retains those names as fallbacks but first attempts to infer an identifier from the initialising expression:

| Initialiser | Inferred name |
| :--- | :--- |
| `require("a.b.c")` | `c` |
| Global alias (`local f = print`) | `print` |
| Table field (`local v = t.field`) | `field` |
| Method call (`local x = t:get()`) | `get` *(constructor-like names excluded)* |
| Table constructor | `tbl` |
| Function body | `fn` |
| Vararg expression | `args` |
| String / number literal | `str` / `num` |
| Boolean literal, comparison, negation | `flag` |
| Length operator | `count` |
| Concatenation | `str` |
| Other arithmetic | `num` |

Loop iterators are named `i`, `j`, `k`, `l` (then `iN`) for numeric loops; `i`, `v` for generic loops over `ipairs`; and `k`, `v` for loops over `pairs` or `next`.

If the first parameter of a function is used as a table or method receiver, it is named `self`, and the source writer emits the definition in method form.

All inferred names are sanitised against Lua's identifier rules and keyword list, and uniquified against a per-function registry seeded with parameter names, upvalue names and, where present, debug names, so inference cannot introduce shadowing.

## Usage

1. Run the program:
   ```bash
   luajit-decompiler-v2.exe INPUT_PATH [options]
   ```
2. All successfully decompiled `.lua` files are placed by default into the `output` folder located in the same directory as the executable, mirroring the input directory structure.

**Available options:**

| Option | Effect |
| :--- | :--- |
| `-h`, `-?`, `--help` | Show usage and options. |
| `-o`, `--output OUTPUT_PATH` | Override the default output directory. |
| `-e`, `--extension EXTENSION` | Only decompile files with the specified extension. |
| `-s`, `--silent_assertions` | Disable assertion pop-ups and automatically skip failing files. |
| `-f`, `--force_overwrite` | Always overwrite existing output files. |
| `-i`, `--ignore_debug_info` | Ignore bytecode debug information. |
| `-m`, `--minimize_diffs` | Optimise output formatting to help minimise diffs. |
| `-u`, `--unrestricted_ascii` | Disable default UTF-8 encoding and string restrictions. |

## Output conventions

- Each emitted file begins with a `-- chunkname:` comment when the bytecode carries a chunk name.
- With debug information, original variable names are used. Without it, heuristic names are used where inferable, otherwise positional names (`var_F_N`, `arg_F_N`, `iter_F_N`), where `F` is the function identifier (or the nesting level under `-m`).
- Residual unstructured jumps are emitted as `goto label_F_N` with matching label definitions.
- A UTF-8 byte order mark is written unless `-u` is specified.

## Limitations

- Little-endian bytecode only.
- Boolean regions whose graph cannot be made monochromatic (untwistable DAGs in the terminology of the paper) are currently rejected; the affected file is skipped rather than partially restructured.
- Loop header conditions are structured by the loop pass, not by the boolean graph.
- Value-preserving short-circuit expressions (`a and b` used as a value rather than as a condition) are reconstructed only for the assignment patterns recognised by the assignment pass.

## TODO

- Bytecode big-endian support (carried from upstream).
- Improved decompilation logic for conditional assignments (carried from upstream).
- Node splitting for untwistable boolean DAGs (paper, Section 6), to recover the currently rejected regions.
- Folding of negated comparisons into complementary Lua operators (`not (a < b)` to `a >= b`) during expression emission.
- Recovery of value-based `and`/`or` from `ISTC`/`ISFC` instruction pairs.
- Normalisation of LuaJIT 2.1 trace-oriented opcodes (type assertions, restricted table accesses, loop opcode variants) where present in inputs.
- Parallel processing of independent input files.
- A round-trip verification harness (decompile, recompile, compare).

## References

- M. G. Nanda, S. Arun-Kumar. *Decompiling Boolean Expressions from Java Bytecode.* ISEC 2016. [www.cse.iitd.ac.in/~sak/reports/isec2016-paper.pdf](https://www.cse.iitd.ac.in/~sak/reports/isec2016-paper.pdf)
- Original project: [marsinator358/luajit-decompiler-v2](https://github.com/marsinator358/luajit-decompiler-v2)
- Fork base: [Aussiemon/luajit-decompiler-v2](https://github.com/Aussiemon/luajit-decompiler-v2)
