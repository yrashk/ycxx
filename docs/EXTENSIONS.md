# Extensions

DECISIONS §1.2: ycxx accepts an extension only because a system header or libycxx needs it. Each
one is listed here with the headers that need it and what ycxx does with it. Nothing is added for
its own sake.

| Extension | Needed by | What ycxx does | Since |
| --- | --- | --- | --- |
| GNU attributes `__attribute__((…))`, before a declaration, among its decl-specifiers, and after a declarator | the macOS SDK's `<stdio.h>` family (`__printflike`, `__deprecated_msg`, `__swift_nonisolated_unsafe`), `sys/cdefs.h` | Parsed and ignored, as `[[…]]` attributes it does not know are ([dcl.attr.grammar]/6) | #133 |
| `asm` labels `__asm("…")`, `__asm__("…")`, `asm("…")` after a declarator | the macOS SDK's `__DARWIN_ALIAS` and `__DARWIN_ALIAS_STARTING` (`fopen`, `fwrite`, …) | The string is the entity's symbol, exactly as written, in place of its linkage name | #133 |

Standard features that are parsed and not yet acted on, which are not extensions:

- `[[…]]` attribute-specifiers on declarations and statements are skipped
  ([dcl.attr.grammar]/6). `[[gnu::visibility]]` and the other attributes libycxx's `config.hpp`
  uses are therefore without effect for now.
