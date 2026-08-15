#pragma once

// Registers the `passwd` command and, when a secret is already stored, starts
// the TCP console listener. Call once from app_main after consoleStart() and
// after the network stack exists. Without a stored secret nothing is bound.
void netconsoleBegin();
