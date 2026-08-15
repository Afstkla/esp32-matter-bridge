#pragma once

// The speaker, the codec and I2S exist only while a beep session runs:
// audioBeep(true) powers the chain up, audioBeep(false) tears all of it down
// again. Nothing here is safe to call before audioBegin().
void audioBegin();
void audioBeep(bool on);
