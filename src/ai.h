// ============================================================================
//  ai.h :: "Ask your PC" — async LLM call over WinHTTP (no dependencies)
//          Works with Anthropic Messages API or OpenAI-compatible chat APIs.
// ============================================================================
#pragma once
#include "app.h"
#include <string>

// Build a compact, human-readable system snapshot to feed the model.
std::string aiBuildContext(const Metrics& m);

// Kick off a request on a background thread (no-op if one is already running).
// `title`  shows in the result panel header; `userPrompt` is the question;
// `context` is the metrics snapshot (built on the caller's thread).
void aiAsk(const Config& cfg, const std::string& title,
           const std::string& userPrompt, const std::string& context);

AiView aiSnapshot();   // thread-safe copy for the renderer
bool    aiBusy();      // true while a request is in flight
void    aiDismiss();   // clear the result panel (back to idle)
