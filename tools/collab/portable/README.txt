Collab fork of MuseScore Studio - test build (not an official MuseScore release)
=============================================================================

Real-time collaboration: everyone sees each other's edits, cursors and
selections live. Measures someone is working in are locked for others.

First, once: double-click Join.bat in this folder and close the window it
asks in. That lets "Open in Ploq" links open this app.

Then everything happens on the website, https://collab.ploq.app :

  Share a score   Type your name, upload a .mscz, and you get an invite link
                  (copied for you). Text it to whoever you want to edit with.
                  "Open in Ploq" opens the score in this app.
  Join a score    Open the invite link you were sent, type your name, click
                  "Join and open in Ploq".

Also handy
  Host.bat        Share a score straight from this computer: drag a .mscz onto
                  it. The app copies an invite link; Ctrl+Alt+I copies another.

Notes
- Windows 10/11 64-bit. Nothing is installed; delete the folder to remove it.
  "ploq://" links are registered for your Windows user only.
- Only people you've sent an invite link to can open your score.
- Changing instruments, measures or time/key signatures sends the whole score;
  everyone else's window reloads it.
- Your copy of a shared score is kept in MuseScore's data folder; use Save As
  to keep your own copy.

This is free software under the GNU GPL v3 (see LICENSE.txt), based on
MuseScore Studio (https://github.com/musescore/MuseScore).
