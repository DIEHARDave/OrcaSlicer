#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <map>
#include <string>

namespace Slic3r {

// Flashforge printers running Zmod (https://github.com/ghzserg/zmod) without the native screen:
// the Creator 5 / 5 Pro tool changer and the AD5X with its material station. Filament comes from
// Zmod's spool slots; a print starts through Zmod's PRINT_ZCOLOR, which assigns the file's tools to
// slots and takes the dialog's bed leveling and Auto PA choices.
class ZmodPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit ZmodPrinterAgent(std::string log_dir);
    ~ZmodPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

protected:
    int  start_uploaded_print(const PrintParams& params, const std::string& filename) override;
    bool supports_flow_calibration() const override { return true; }
};

} // namespace Slic3r
