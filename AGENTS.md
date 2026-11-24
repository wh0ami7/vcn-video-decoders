# Project instructions for Grok

## MCP-first code intelligence (required)

Prefer MCP tools over ad-hoc `grep` / `find` / raw `Read` loops when working in this repo. Call `search_tool` when you need an MCP tool’s input schema.

### Lookups → Codegraph MCP

Use **Codegraph** for discovery, navigation, and understanding before you edit:

| Need | Tool |
|------|------|
| Default: how does X work, where is X, architecture, bug context, symbols about to change | `codegraph_explore` |
| Read one file or one named symbol (instead of Read when indexed) | `codegraph_node` |
| Callers / callees / blast radius before a refactor | `codegraph_callers`, `codegraph_callees`, `codegraph_impact` |
| Symbol name search (locations only) | `codegraph_search` |
| Project layout | `codegraph_files` |
| Index health | `codegraph_status` |

Treat Codegraph-returned source as already read. Do not re-open the same files with Read unless the index is stale or the path is outside the graph.

### Read / Write → Serena MCP

Use **Serena** for filesystem and edit work:

1. Call `serena__initial_instructions` once per session if you do not yet have a Serena session id.
2. Drive everything through `serena__serena_repl` with that session and `s.*` facades.
3. Before using a facade, call `s.info("<facade>")` (or `s.info("fs", "edit", "lsp", "mem")` as needed).

Typical Serena uses:

- **Read / search:** `s.fs.read_file`, `s.fs.search_for_pattern`, `s.fs.find_file`, `s.fs.list_dir`
- **Write / edit:** `s.edit.replace_content`, `s.edit.replace_lines`, and related edit helpers from `s.info("edit")`
- **Symbols / diagnostics:** `s.lsp.*` when you need live LSP views Serena exposes
- **Project memory:** `s.mem.*` for Serena memories (e.g. `security`)

Prefer Serena for creating and patching source files in this tree. Keep shell/`Write` for ops Serena cannot do (builds, git push, package installs).

### Use every MCP capability available

When MCP servers are connected, use them for their purpose instead of inventing workarounds:

| Server | Use for |
|--------|---------|
| **codegraph** | Lookups, explore, impact, file/symbol reads from the index |
| **serena** | Project read/write, search, LSP/memory via REPL |
| **graphify** | Architecture / knowledge-graph questions when `graphify-out/` or graphify tools apply |
| **rea** | Reverse engineering of binaries, local artifacts, native/managed code, ADB, WebMCP discovery |
| **tasks** | Task tracking if the tasks server is connected |
| Other connected MCPs | Prefer the specialist tool (`search_tool` first) over reinventing with shell |

Do not skip an available MCP tool because a weaker shell/grep path exists. Parallelize independent MCP calls.

## Repo conventions (short)

- x86_64 macOS; build via `*/build.sh` and `installer/build-app.sh`
- GPU work is dangerous: keep validated-driver gates, VCN0 for AV1, and exact GPU allow env strings
- Encoding fix lives under `vcnencodefix/`; optional in the installer
- Prefer existing project style; run builds/tests after non-trivial C/C++ edits when feasible
