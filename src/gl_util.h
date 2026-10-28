#pragma once

// Builds a shader program; prints the compiler log and returns 0 on failure.
unsigned linkProgram(const char* vertexSource, const char* fragmentSource);
