// crash.h -- records crashes in %APPDATA%\Ultima III Assistant\crashes.
#pragma once

namespace crash {

// Installs handlers that, on an unhandled exception or std::terminate, append
// the exception, the faulting module and a stack trace to crash.log and write
// a minidump beside it. Windows still gets to report the crash afterwards.
void Install();

}  // namespace crash
