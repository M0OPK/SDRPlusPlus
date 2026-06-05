#include "utils/event.h"
#include <utils/proto/rigctl.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <recorder_interface.h>
#include <meteor_demodulator_interface.h>
#include <config.h>
#include <cctype>
#include <radio_module.h>
#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rigctl_client",
    /* Description:     */ "Client for the RigCTL protocol",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class RigctlClientModule : public ModuleManager::Instance {
public:
    RigctlClientModule(std::string name) {
        this->name = name;

        // Load default
        strcpy(host, "127.0.0.1");

        // Load config
        config.acquire();
        if (config.conf[name].contains("host")) {
            std::string h = config.conf[name]["host"];
            strcpy(host, h.c_str());
        }
        if (config.conf[name].contains("port")) {
            port = config.conf[name]["port"];
            port = std::clamp<int>(port, 1, 65535);
        }
        if (config.conf[name].contains("ifFreq")) {
            ifFreq = config.conf[name]["ifFreq"];
        }

        if (config.conf[name].contains("fm_offset")) {
            fm_offset = config.conf[name]["fm_offset"];
        }
        if (config.conf[name].contains("am_offset")) {
            am_offset = config.conf[name]["am_offset"];
        }
        if (config.conf[name].contains("cw_offset")) {
            cw_offset = config.conf[name]["cw_offset"];
        }
        if (config.conf[name].contains("lsb_offset")) {
            lsb_offset = config.conf[name]["lsb_offset"];
        }
        if (config.conf[name].contains("usb_offset")) {
            usb_offset = config.conf[name]["usb_offset"];
        }

        config.release();

        _retuneHandler.ctx = this;
        _retuneHandler.handler = retuneHandler;
        _modChangeHandler.ctx = this;
        _modChangeHandler.handler = modChangeHandler;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~RigctlClientModule() {
        stop();
        gui::menu.removeEntry(name);
    }

    void postInit() {
        
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (running) { return; }

        // Connect to rigctl server
        try {
            client = net::rigctl::connect(host, port);
        }
        catch (const std::exception& e) {
            flog::error("Could not connect: {}", e.what());
            return;
        }

        // Switch source to panadapter mode
        sigpath::sourceManager.setPanadapterIF(ifFreq);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::PANADAPTER);
        sigpath::sourceManager.onRetune.bindHandler(&_retuneHandler);

        // Get mode changes
        if (RadioModule * radioMod = (RadioModule *)core::moduleManager.getInterface("", "RadioModule"))
        {
            radioMod->onModeChanged.bindHandler(&_modChangeHandler);
            net::rigctl::Mode mode = RigctlClientModule::getMode((DemodID)radioMod->getSelectedDemodId());
            this->setMode(mode);
        }

        running = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!running) { return; }

        // Switch source back to normal mode
        sigpath::sourceManager.onRetune.unbindHandler(&_retuneHandler);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::NORMAL);
        if (RadioModule * radioMod = (RadioModule *)core::moduleManager.getInterface("", "RadioModule"))
        {
            radioMod->onModeChanged.unbindHandler(&_modChangeHandler);
        }

        // Disconnect from rigctl server
        client->close();

        running = false;
    }

    int setMode(net::rigctl::Mode mode)
    {
        double mode_offset = 0.0l;
        int result = this->client->setMode(mode);
        if (!result)
        {
            switch (mode)
            {
                case net::rigctl::Mode::MODE_AM:
                    mode_offset = this->am_offset;
                    break;
                case net::rigctl::Mode::MODE_FM:
                    mode_offset = this->fm_offset;
                    break;
                case net::rigctl::Mode::MODE_CW:
                    mode_offset = this->cw_offset;
                    break;
                case net::rigctl::Mode::MODE_LSB:
                    mode_offset = this->lsb_offset;
                    break;
                case net::rigctl::Mode::MODE_USB:
                    mode_offset = this->usb_offset;
                    break;
                default:
                    break;
            }

            sigpath::sourceManager.setPanadapterOffset(mode_offset);
        }
        return result;
    }

private:
    static void menuHandler(void* ctx) {
        RigctlClientModule* _this = (RigctlClientModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (_this->running) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_rigctl_cli_host_", _this->name), _this->host, 1023)) {
            config.acquire();
            config.conf[_this->name]["host"] = std::string(_this->host);
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_rigctl_cli_port_", _this->name), &_this->port, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }
        if (_this->running) { style::endDisabled(); }

        ImGui::LeftLabel("IF Frequency");
        ImGui::FillWidth();
        if (ImGui::InputDouble(CONCAT("##_rigctl_if_freq_", _this->name), &_this->ifFreq, 100.0, 100000.0, "%.0f")) {
            if (_this->running) {
                sigpath::sourceManager.setPanadapterIF(_this->ifFreq);
            }
            config.acquire();
            config.conf[_this->name]["ifFreq"] = _this->ifFreq;
            config.release(true);
        }

        ImGui::FillWidth();
        if (_this->running && ImGui::Button(CONCAT("Stop##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!_this->running && ImGui::Button(CONCAT("Start##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->start();
        }

        if (_this->offset_param_edit) {
            bool valid = false;
            _this->offset_param_edit = _this->offsetParamMenu(valid);

            // If the menu was closed and (TODO) valid, update options
            if (!_this->offset_param_edit && valid) {
                _this->am_offset = _this->_am_offset;
                _this->fm_offset = _this->_fm_offset;
                _this->cw_offset = _this->_cw_offset;
                _this->lsb_offset = _this->_lsb_offset;
                _this->usb_offset = _this->_usb_offset;

                config.acquire();
                config.conf[_this->name]["fm_offset"] = _this->fm_offset;
                config.conf[_this->name]["am_offset"] = _this->am_offset;
                config.conf[_this->name]["cw_offset"] = _this->cw_offset;
                config.conf[_this->name]["lsb_offset"] = _this->lsb_offset;
                config.conf[_this->name]["usb_offset"] = _this->usb_offset;
                config.release(true);
            }
        }

        ImGui::FillWidth();
        if (ImGui::Button(CONCAT("Offsets##rigctl_offset_edit_btn", _this->name), ImVec2(menuWidth, 0))) {
            _this->offset_param_edit = true;
            _this->_fm_offset = _this->fm_offset;
            _this->_am_offset = _this->am_offset;
            _this->_cw_offset = _this->cw_offset;
            _this->_lsb_offset = _this->lsb_offset;
            _this->_usb_offset = _this->usb_offset;
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (_this->client && _this->client->isOpen() && _this->running) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Connected");
        }
        else if (_this->client && _this->running) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Disconnected");
        }
        else {
            ImGui::TextUnformatted("Idle");
        }
    }

    bool offsetParamMenu(bool& valid) {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;
        ImGui::OpenPopup("Edit##rigctl_edit_offset_params_");
        if (ImGui::BeginPopup("Edit##rigctl_edit_offset_params_", ImGuiWindowFlags_NoResize)) {
            if (ImGui::BeginTable(("rigctl_offset_param_tbl" + name).c_str(), 2)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("FM");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("hz##rigctl_offset_fm", &_fm_offset);
                _fm_offset = std::clamp<int>(_fm_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("AM");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("hz##rigctl_offset_am", &_am_offset);
                _am_offset = std::clamp<int>(_am_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("CW");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("hz##rigctl_offset_cw", &_cw_offset);
                _cw_offset = std::clamp<int>(_cw_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("LSB");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("hz##rigctl_offset_lsb", &_lsb_offset);
                _lsb_offset = std::clamp<int>(_lsb_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("USB");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("hz##rigctl_offset_usb", &_usb_offset);
                _usb_offset = std::clamp<int>(_usb_offset, -10000, 10000);

                ImGui::EndTable();
            }

            if (ImGui::Button(" Apply ")) {
                open = false;
                valid = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
                valid = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }


    static void retuneHandler(double freq, void* ctx) {
        RigctlClientModule* _this = (RigctlClientModule*)ctx;
        if (!_this->client || !_this->client->isOpen()) { return; }
        if (_this->client->setFreq(freq)) {
            flog::error("Could not set frequency");
        }
    }

    static void modChangeHandler(DemodID modId, void* ctx) {
        RigctlClientModule* _this = (RigctlClientModule*)ctx;
        if (!_this->client || !_this->client->isOpen()) { return; }

        net::rigctl::Mode mode = getMode(modId);
        if (mode != net::rigctl::MODE_INVALID)
        {
            _this->setMode(mode);
        }
    }

    static net::rigctl::Mode getMode(DemodID modId)
    {
        net::rigctl::Mode mode = net::rigctl::Mode::MODE_INVALID;
        switch (modId)
        {
            case DemodID::RADIO_DEMOD_AM:
                return net::rigctl::Mode::MODE_AM;
            case DemodID::RADIO_DEMOD_CW:
                return net::rigctl::Mode::MODE_CW;
            case DemodID::RADIO_DEMOD_DSB:
                return net::rigctl::Mode::MODE_DSB;
            case DemodID::RADIO_DEMOD_LSB:
                return net::rigctl::Mode::MODE_LSB;
            case DemodID::RADIO_DEMOD_NFM:
                return net::rigctl::Mode::MODE_FM;
            case DemodID::RADIO_DEMOD_RAW:
                return net::rigctl::Mode::MODE_INVALID;
            case DemodID::RADIO_DEMOD_USB:
                return net::rigctl::Mode::MODE_USB;
            case DemodID::RADIO_DEMOD_WFM:
                return net::rigctl::Mode::MODE_WFM;
            default:
                return net::rigctl::MODE_INVALID;
        }
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    std::recursive_mutex mtx;

    char host[1024];
    int port = 4532;
    std::shared_ptr<net::rigctl::Client> client;

    double ifFreq = 8830000.0;

    int offset_param_edit = false;
    int fm_offset = 0;
    int am_offset = 0;
    int cw_offset = 0;
    int lsb_offset = 0;
    int usb_offset = 0;

    int _fm_offset = 0;
    int _am_offset = 0;
    int _cw_offset = 0;
    int _lsb_offset = 0;
    int _usb_offset = 0;

    EventHandler<double> _retuneHandler;
    EventHandler<DemodID> _modChangeHandler;
};

MOD_EXPORT void _INIT_() {
    config.setPath(std::string(core::getRoot()) + "/rigctl_client_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RigctlClientModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (RigctlClientModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
