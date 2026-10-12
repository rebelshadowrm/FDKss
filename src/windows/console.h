#pragma once
#include "fdkss/api.h"
namespace live_console {
bool start();
void pump(fdkss_runtime *);
void stop();
}
