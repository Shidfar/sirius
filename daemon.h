/* daemon.h - `sirius run`: waits for the button, records, transcribes, encrypts, uploads, keeps the display up to date. */
#pragma once

int daemon_main(int argc, char **argv);
/* `sirius ctl toggle|sync`: tells the running daemon to start/stop a recording, or to sync now (same as the button). */
int ctl_main(int argc, char **argv);
