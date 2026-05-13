// Pyxis app — `--variant` CLI parser implementation.

#include "VariantParser.h"

#include <cctype>
#include <utility>

namespace pyxis::app {

namespace {

[[nodiscard]] std::string_view Trim(std::string_view view) noexcept
{
  while (!view.empty() && std::isspace(static_cast<unsigned char>(view.front())))
    view.remove_prefix(1);
  while (!view.empty() && std::isspace(static_cast<unsigned char>(view.back())))
    view.remove_suffix(1);
  return view;
}

}  // namespace

std::vector<VariantSelection> ParseVariantSelections(std::string_view spec) noexcept
{
  std::vector<VariantSelection> out;
  std::size_t cursor = 0;
  while (cursor < spec.size())
  {
    const std::size_t comma = spec.find(',', cursor);
    const std::string_view chunk = Trim(spec.substr(
        cursor,
        (comma == std::string_view::npos) ? std::string_view::npos : (comma - cursor)));
    cursor = (comma == std::string_view::npos) ? spec.size() : (comma + 1u);
    if (chunk.empty())
      continue;

    // Split on the FIRST ':' (path) and the FIRST '=' after that (set).
    // Variant values are constrained by USD to TfMakeValidIdentifier
    // so they can't contain ':' or '=' themselves — splitting on the
    // first occurrence is safe.
    const std::size_t colon = chunk.find(':');
    if (colon == std::string_view::npos || colon == 0u)
      continue;
    const std::size_t equalsPos = chunk.find('=', colon + 1u);
    if (equalsPos == std::string_view::npos)
      continue;

    const std::string_view pathView  = Trim(chunk.substr(0u, colon));
    const std::string_view setView   = Trim(chunk.substr(colon + 1u, equalsPos - colon - 1u));
    const std::string_view valueView = Trim(chunk.substr(equalsPos + 1u));
    if (pathView.empty() || pathView.front() != '/'
        || setView.empty() || valueView.empty())
      continue;

    VariantSelection record;
    record.primPath.assign(pathView);
    record.setName.assign(setView);
    record.value.assign(valueView);
    out.push_back(std::move(record));
  }
  return out;
}

}  // namespace pyxis::app
