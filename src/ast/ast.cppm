// The AST core in one import (docs/design/ast.md): handles, the node schema, records, interners,
// frame builders, the red view and compaction.
export module ycxx.ast;

export import ycxx.ast.fingerprint;
export import ycxx.ast.handles;
export import ycxx.ast.schema;
export import ycxx.ast.nodes;
export import ycxx.ast.records;
export import ycxx.ast.interner;
export import ycxx.ast.builder;
export import ycxx.ast.red;
export import ycxx.ast.compact;
