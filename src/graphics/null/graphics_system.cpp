/**
 * @file        graphics/null/graphics_system.cpp
 * @brief       GPU backend that executes the command stream without a host GPU
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/null/command_processor.h>
#include <rex/graphics/null/graphics_system.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/software/provider.h>

namespace rex::graphics::null {

NullGraphicsSystem::NullGraphicsSystem() {}

NullGraphicsSystem::~NullGraphicsSystem() {}

std::string NullGraphicsSystem::name() const {
  return "Null";
}

void NullGraphicsSystem::CreateProvider(bool with_presentation) {
  // The software provider needs nothing from the host, so headless and
  // presenting setups use the same one.
  (void)with_presentation;
  provider_ = rex::ui::software::SoftwareProvider::Create();
}

std::unique_ptr<CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::null
