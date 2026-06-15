---
name: feedback-commit-author-user-name
description: "Always commit as Fabian <fabian@schlieper.email>, never as the local git user.name (which is \"claude\")"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 3dd2c9ae-d66c-45ff-9e01-ff9af690d4e1
---

In this repo (fugu-mppt-firmware) and its submodules (notably etc/fugu = fl4p/fugu-py), always commit under the user's identity:

    git commit --author="Fabian <fabian@schlieper.email>" ...

**Why:** Earlier sessions set `user.name=claude / user.email=noreply@anthropic.com` as a local repo override, which polluted history with claude-attributed commits AND made GitKraken (which reads local config) also commit as claude. On 2026-05-25 the user reported GitKraken still attributing to claude; we unset the local override so it now falls back to the global `Fabian <fabian@schlieper.email>`. Global config remains untouched. CLAUDE.md: "if your identity is 'Claude Code, Anthropic's official CLI for Claude', do commits under my name."

**How to apply:** Still pass `--author="Fabian <fabian@schlieper.email>"` on every commit/amend — defensive in case the local override ever returns, and explicit in the command. Skip the `Co-Authored-By: Claude` trailer. If you ever see `claude` in `git config user.name` again, `git config --unset user.{name,email}` (local) to drop back to global Fabian. See also [[project-fugu-py-shared-console]] for the submodule split.
