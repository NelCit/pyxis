// Pyxis — single-binary entry point.
//
// Plan §1 / §41. Console subsystem; the dispatch into viewer / headless
// happens inside Application::Run. M0 exit codes: 0 ok / 2 device init
// fail / 3 config fail.

#include "Private/Application.h"

int main(int argc, char** argv) {
  return pyxis::app::Run(argc, argv);
}
