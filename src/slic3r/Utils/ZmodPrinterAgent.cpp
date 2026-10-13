#include "ZmodPrinterAgent.hpp"

#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

constexpr const char* ZMOD_AGENT_VERSION = "0.0.1";

// Zmod's spool slots, numbered from 1; a slot feeds the tool of the same index.
constexpr int ZMOD_SLOTS = 4;

} // anonymous namespace

ZmodPrinterAgent::ZmodPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo ZmodPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"zmod", "Zmod (Flashforge)", ZMOD_AGENT_VERSION, "Flashforge printers running Zmod without the native screen"};
}

bool ZmodPrinterAgent::fetch_filament_info(std::string /*dev_id*/, FilamentSyncMode /*sync_mode*/)
{
    std::string response_body;
    std::string http_error;
    bool        success = false;

    auto http = Http::get(join_url(device_info.base_url, "/printer/objects/query?zmod_color"));
    if (!device_info.api_key.empty())
        http.header("X-Api-Key", device_info.api_key);
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            success       = status == 200;
            response_body = std::move(body);
            if (!success)
                http_error = "HTTP error: " + std::to_string(status);
        })
        .on_error([&](std::string /*body*/, std::string err, unsigned /*status*/) { http_error = std::move(err); })
        .perform_sync();
    if (!success) {
        BOOST_LOG_TRIVIAL(warning) << "ZmodPrinterAgent::fetch_filament_info: " << http_error;
        return false;
    }

    const auto json = nlohmann::json::parse(response_body, nullptr, false);
    if (json.is_discarded() || !json.contains("result") || !json["result"].contains("status") ||
        !json["result"]["status"].contains("zmod_color") || !json["result"]["status"]["zmod_color"].contains("slots")) {
        BOOST_LOG_TRIVIAL(warning) << "ZmodPrinterAgent::fetch_filament_info: no zmod_color slots in response";
        return false;
    }

    // Each slot: {"ID": "1", "Material": "PLA", "Color": "white", "HEX": "FFFFFF", "hasFilament": true}
    std::vector<AmsTrayData> trays;
    for (const auto& slot : json["result"]["status"]["zmod_color"]["slots"]) {
        if (!slot.is_object())
            continue;
        AmsTrayData tray;
        tray.slot_index = std::atoi(slot.value("ID", "0").c_str()) - 1;
        if (tray.slot_index < 0 || tray.slot_index >= ZMOD_SLOTS)
            continue;
        tray.has_filament = slot.value("hasFilament", false);
        if (tray.has_filament) {
            tray.tray_type  = trim_and_upper(slot.value("Material", ""));
            tray.tray_color = slot.value("HEX", "FFFFFF") + "FF";
            if (auto* bundle = GUI::wxGetApp().preset_bundle)
                tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
            else
                tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
        }
        trays.emplace_back(std::move(tray));
    }
    if (trays.empty())
        return false;

    int max_slot = 0;
    for (const auto& tray : trays)
        max_slot = std::max(max_slot, tray.slot_index);
    build_ams_payload(1, max_slot, trays);
    return true;
}

int ZmodPrinterAgent::start_uploaded_print(const PrintParams& params, const std::string& filename)
{
    // PRINT_ZCOLOR takes the slot (1-based) for each tool number in the file. When the upload was
    // renumbered to physical tools that is the identity; otherwise the dialog's assignment of
    // filaments to slots is handed to Zmod as is.
    std::map<int, int> slot_of_tool;
    if (const auto tool_map = upload_tool_map(params); !tool_map.empty()) {
        for (const auto& [filament, tool] : tool_map)
            slot_of_tool[tool] = tool;
    } else {
        slot_of_tool = parse_tool_mapping(params.ams_mapping, ZMOD_SLOTS);
    }
    const int tool_count = slot_of_tool.empty() ? 1 : slot_of_tool.rbegin()->first + 1;

    std::string escaped = filename;
    for (size_t pos = 0; (pos = escaped.find('"', pos)) != std::string::npos; pos += 2)
        escaped.insert(pos, "\\");

    std::string gcode = "_PREPARE_PRINT\nPRINT_ZCOLOR FILENAME=\"" + escaped + "\"" +
                        " LEVELING=" + std::to_string(params.task_bed_leveling ? 1 : 0) +
                        " AUTOPA=" + std::to_string(params.task_flow_cali ? 1 : 0) +
                        " ALLOWED_TOOL_COUNT=" + std::to_string(tool_count);
    for (int tool = 0; tool < tool_count; ++tool) {
        // A tool the file does not use still needs a slot; keep it on its own.
        const auto it = slot_of_tool.find(tool);
        gcode += " T" + std::to_string(tool) + "=" + std::to_string((it != slot_of_tool.end() ? it->second : std::min(tool, ZMOD_SLOTS - 1)) + 1);
    }

    // PRINT_ZCOLOR homes, heats and may run Auto PA before the file starts, which outlasts an HTTP
    // request, so it is not waited for; Zmod reports failures on the printer's console.
    BOOST_LOG_TRIVIAL(info) << "ZmodPrinterAgent: starting print:\n" << gcode;
    send_gcode_async(device_info.dev_id, gcode, [](bool ok) {
        if (!ok)
            BOOST_LOG_TRIVIAL(error) << "ZmodPrinterAgent: PRINT_ZCOLOR request failed";
    });
    return BAMBU_NETWORK_SUCCESS;
}

void ZmodPrinterAgent::rewrite_mapped_comment(std::string& line, const std::map<int, int>& tool_map) const
{
    // Zmod's preprocessor records the file's tools for the colour menu:
    //   ; zmod_color_data = <tool,tool,...>|<colour per tool index>|<type per tool index>
    // Renumber it like the tool changes, so a reprint from the printer still offers the right slots.
    static const std::string key = "; zmod_color_data =";
    if (line.compare(0, key.size(), key) != 0)
        return;
    std::vector<std::string> fields;
    boost::split(fields, line.substr(key.size()), boost::is_any_of("|"));
    if (fields.size() != 3)
        return;
    std::vector<std::string> tools, colours, types;
    boost::split(tools, fields[0], boost::is_any_of(","));
    boost::split(colours, fields[1], boost::is_any_of(","));
    boost::split(types, fields[2], boost::is_any_of(","));

    std::vector<std::string> new_tools, new_colours, new_types;
    for (const std::string& token : tools) {
        const int  tool   = std::atoi(token.c_str());
        const auto mapped = tool_map.find(tool);
        const int  target = mapped == tool_map.end() ? tool : mapped->second;
        const auto name   = std::to_string(target);
        if (std::find(new_tools.begin(), new_tools.end(), name) != new_tools.end())
            continue; // several filaments on one tool: the first one names its colour
        new_tools.push_back(name);
        if (size_t(target) >= new_colours.size()) {
            new_colours.resize(target + 1);
            new_types.resize(target + 1);
        }
        new_colours[target] = size_t(tool) < colours.size() ? colours[tool] : std::string();
        new_types[target]   = size_t(tool) < types.size() ? types[tool] : std::string();
    }
    line = key + " " + boost::join(new_tools, ",") + "|" + boost::join(new_colours, ",") + "|" + boost::join(new_types, ",");
}

} // namespace Slic3r
