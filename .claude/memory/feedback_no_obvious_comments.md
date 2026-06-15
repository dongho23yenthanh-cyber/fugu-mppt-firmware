---
name: feedback_no_obvious_comments
description: "don't add code comments that restate what a name/call already conveys — omit, don't annotate"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 24a2ff30-daec-4d5f-8c62-68b491e18bb1
---

The user dislikes inline comments that state the obvious. Concretely: a `printf_mux(...)` call annotated with `// printf_mux mirrors to telnet/MQTT, raw printf does not` — they wanted the comment gone entirely, not shortened.

**Why:** the name `printf_mux` already conveys the muxing behavior; the comment adds nothing and the trailing contrast is editorializing. Reinforces CLAUDE.md's "keep code comments at a minimum and short."

**How to apply:** before writing an inline comment, ask whether a reader already knows it from the identifier/context. If yes, write no comment. Default to zero comments; only annotate genuinely non-obvious intent. Don't justify my own implementation choices in comments. Relates to [[project_perf_h_noninline_odr.md]].
