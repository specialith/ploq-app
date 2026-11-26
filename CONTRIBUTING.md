# Contributing to Ploq

Thanks for helping. Ploq is a small project, so a few notes keep things easy:

- **Bugs:** open an issue with what you did, what you expected, and what happened. Ploq's log
  files are in `%LOCALAPPDATA%\Ploq\Ploq\logs`; attach the one from the session where it went
  wrong (check it for anything private first).
- **Security problems:** report them privately through **Security > Report a vulnerability** on
  this repository, not in a public issue.
- **Pull requests:** keep each one to one change, and say why it's needed. Collaboration code
  lives in `src/collab/`. Changes to MuseScore's own files should stay small and be marked with a
  `COLLAB:` comment, so Ploq can keep merging upstream MuseScore releases.
- **Code style:** follow MuseScore's
  [code guidelines](https://github.com/musescore/muse_framework/wiki/CodeGuidelines).
- **License:** by contributing, you agree that your contribution is licensed under the GNU GPL
  version 3, like the rest of Ploq.

Problems that also happen in MuseScore Studio itself are best reported to
[MuseScore](https://github.com/musescore/MuseScore/issues).
