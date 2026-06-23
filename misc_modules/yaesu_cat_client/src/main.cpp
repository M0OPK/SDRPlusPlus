#include <async_comm/serial.h>
#include "gui/tuner.h"
#include "signal_path/source.h"
#include "utils/event.h"
#include "utils/flog.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ios>
#include <memory>
#include <mutex>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <radio_module.h>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "yaesu_cat_client",
    /* Description:     */ "Client for two way sync with Yaesu radios",
    /* Author:          */ "M0OPK",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class YaesuCatClientModule : public ModuleManager::Instance {

const long SERIAL_TIMEOUT_MS = 100;

enum YaesuMode
{    
    MODE_LSB = 0x01,
    MODE_USB,
    MODE_CWU,
    MODE_FM,
    MODE_AM,
    MODE_RTTYL,
    MODE_CWL,
    MODE_DATAL,
    MODE_RTTYU,
    MODE_DATAFM,
    MODE_FMN,
    MODE_DATAU,
    MODE_AMN,
    MODE_PSK,
    MODE_DATAFMN,
    MODE_INVALID = 0xff
};

enum YaesuAiMode
{
    MODE_UNSET = 0xff,
    MODE_OFF = 0x00,
    MODE_ON = 0x01,
};

public:
    YaesuCatClientModule(std::string name) {
        this->name = name;

        // Load default
#if defined(_WIN32)
        strcpy(comm_port, "COM1");
#else
        strcpy(comm_port, "/dev/ttyS0");
#endif

        // Load config
        config.acquire();
        if (config.conf[name].contains("comm_port")) {
            std::string h = config.conf[name]["comm_port"];
            strcpy(comm_port, h.c_str());
        }
        if (config.conf[name].contains("baud_rate")) {
            baud_rate = config.conf[name]["baud_rate"];
            baud_rate = std::clamp<int>(baud_rate, 1200, 115200);
        }
        if (config.conf[name].contains("useIfTuning")) {
            useIfTuning = config.conf[name]["useIfTuning"];
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
        _retuneHandler.handler = YaesuCatClientModule::retuneHandler;
        _modChangeHandler.ctx = this;
        _modChangeHandler.handler = YaesuCatClientModule::modChangeHandler;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~YaesuCatClientModule() {
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

        serial_buffer.clear();
        // Connect to serial port if configured
        if (strlen(comm_port) > 0)
        {
            serial = std::make_shared<async_comm::Serial>(comm_port, baud_rate);
            serial->register_receive_callback(std::bind(&YaesuCatClientModule::cat_callback, this, std::placeholders::_1, std::placeholders::_2));
            if (!serial->init())
            {
                flog::error("Failed to initialize serial port on {0} with baud rate {1}", comm_port, baud_rate);
                return;
            }
        }

        // Send command to check Auto info mode, result will be handled in callback
        initial_ai_state = YaesuAiMode::MODE_UNSET;
        yaesu_check_aimode();

        // Switch source to panadapter mode
        sigpath::sourceManager.setPanadapterIF(ifFreq);
        if (useIfTuning)
            sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::PANADAPTER);
        sigpath::sourceManager.onRetune.bindHandler(&_retuneHandler);

        // Get mode changes
        if (RadioModule * radioMod = (RadioModule *)core::moduleManager.getInterface("", "RadioModule"))
        {
            radioMod->onModeChanged.bindHandler(&_modChangeHandler);
            DemodID modId = (DemodID)radioMod->getSelectedDemodId();
            YaesuMode mode = yaesu_setmode(modId);
            setModeOffset(mode);
        }

        running = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!running) { return; }
        
        // Set radio to initial AI mode
        if (initial_ai_state == YaesuAiMode::MODE_OFF)
        {
            yaesu_set_aimode(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));    // Sleep for 100ms just to clear buffer
        }

        // Switch source back to normal mode
        sigpath::sourceManager.onRetune.unbindHandler(&_retuneHandler);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::NORMAL);
        sigpath::sourceManager.setTuningOffset(0.0l);
        if (RadioModule * radioMod = (RadioModule *)core::moduleManager.getInterface("", "RadioModule"))
        {
            radioMod->onModeChanged.unbindHandler(&_modChangeHandler);
        }

        if (serial != nullptr)
        {
            serial->close();
            serial = nullptr;
        }

        running = false;
    }

    bool yaesu_check_aimode()
    {       
        return yaesu_send_command("AI;", 4);
    }

    bool yaesu_set_aimode(bool ai_mode)
    {
        if (ai_mode)
            return yaesu_send_command("AI1;", 5);
        else
            return yaesu_send_command("AI0;", 5);
    }

    bool yaesu_send_command(const char* command, int maxlen = 13)
    {
        if (serial == nullptr)
        {
            flog::error("Serial port not available");
            return false;
        }
        
        int position = 1;

        while (*command != '\0' && position <= maxlen)
        {
            serial->send_byte(*command);
            ++command;
            ++position;
        }
        return true;
    }

    bool yaesu_tune(double freq)
    {
        if (serial == nullptr)
        {
            flog::error("Serial port not available");
            return false;
        }
        if (lastFreq == freq)
            return true;


        int freqHz = (int)freq;
        char command[13];
        sprintf(command, "FA%09d;", freqHz);
        yaesu_send_command(command);
        return true;
    }

    YaesuMode yaesu_setmode(DemodID mode)
    {
        if (serial == nullptr)
        {
            flog::error("Serial port not available");
            return MODE_INVALID;
        }

        YaesuMode yaesu_mode = getYaesuMode(mode);
        int mode_set = (int)yaesu_mode;
        assert(mode_set <= 0xf);

        char command[5];
        sprintf(command, "MD%X;", mode_set);
        yaesu_send_command(command);
        return yaesu_mode;
    }

    YaesuMode getYaesuMode(DemodID mode)
    {
        YaesuMode yaesu_mode = MODE_INVALID;
        switch (mode) 
        {
            case RADIO_DEMOD_CW:
                yaesu_mode = MODE_CWL;
                break;
            case RADIO_DEMOD_AM:
                yaesu_mode = MODE_AMN;
                break;
            case RADIO_DEMOD_LSB:
                yaesu_mode = MODE_LSB;
                break;
            case RADIO_DEMOD_NFM:
                yaesu_mode = MODE_FMN;
                break;
            case RADIO_DEMOD_USB:
                yaesu_mode = MODE_USB;
                break;
            case RADIO_DEMOD_WFM:       // Decide whether to keep this or go with invalid
                yaesu_mode = MODE_FM;
                break;
            default:
                yaesu_mode = MODE_INVALID;
                break;
        }
        return yaesu_mode;
    }

    void cat_callback(const uint8_t* buf, size_t len)
    {
        // If we have data in the buffer and more than 100ms passed since last serial comms, clear the buffer for a fresh start
        std::chrono::steady_clock::time_point time_now = std::chrono::steady_clock::now();
        long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_serial).count();
        if (!serial_buffer.empty() > 0 && elapsed > SERIAL_TIMEOUT_MS)
        {
            flog::info("Serial timeout, clearing buffer of {} bytes", (int64_t)serial_buffer.size());
            serial_buffer.clear();
        }

        // Reset timeout timer
        last_serial = std::chrono::steady_clock::now();

        // Add the waiting data to the buffer
        for (size_t buffer_loc = 0; buffer_loc < len; buffer_loc++)
        {
            serial_buffer.push_back(buf[buffer_loc]);
        }

        // If we have nothing left, nothing for now
        if (serial_buffer.size() < 3)
            return;

        // See if we have a command ending
        if (std::find(serial_buffer.begin(), serial_buffer.end(), ';') == serial_buffer.end())
            return;

        // Construct the command and remove bytes from buffer
        std::string cat_command = "";

        // Take just the actual command data
        while(serial_buffer[0] != ';')
        {
            cat_command.push_back(serial_buffer[0]);
            serial_buffer.erase(serial_buffer.begin());
        }

        // Remove final byte (will be semicolon)
        serial_buffer.erase(serial_buffer.begin());

        // Make sure we have a useful command
        if (cat_command.length() < 2)
            return;

        // Get command prefix (first two characters)
        std::string prefix = cat_command.substr(0, 2);

        // Set mode to invalid. If we set it later, we'll send to SDR++
        YaesuMode mode = YaesuMode::MODE_INVALID;

        // Set frequency to last frequency, so we know if it changes
        int freqHz = (int)lastFreq;

        // Handle AI command result
        if (prefix == "AI" && cat_command.length() == 3)
        {
            char ai_state = *cat_command.substr(2, 1).c_str();
            if (ai_state == '0')
            {
                initial_ai_state = YaesuAiMode::MODE_OFF;
                yaesu_set_aimode(true);
            }
            else if (ai_state == '1')
            {
                initial_ai_state = YaesuAiMode::MODE_ON;
            }
            else 
            {
                flog::error("AI result received with invalid state {0}", ai_state);
            }
        }
        // Handle general info message (contains frequency and mode)
        else if (prefix == "IF" && cat_command.length() == 27)
        {
            // Extract frequency and mode only
            std::string freqHzStr = cat_command.substr(5, 9);
            std::string modeId_hex = cat_command.substr(21,1);
            freqHz = std::stoi(freqHzStr);
            int modeId = 0;
            std::sscanf(modeId_hex.c_str(), "%X", &modeId);
            mode = (YaesuMode)modeId;
        }
        // Handle frequency change
        else if (prefix == "FA" && cat_command.length() == 11)
        {
            // Extract frequency
            std::string freqHzStr = cat_command.substr(2, 9);
            freqHz = std::stoi(freqHzStr);            
        }
        // Handle mode change
        else if (prefix == "MD" && cat_command.length() == 4)
        {
            // Extract mode
            std::string modeId_hex = cat_command.substr(3,1);
            int modeId = 0;
            std::sscanf(modeId_hex.c_str(), "%X", &modeId);
            mode = (YaesuMode)modeId;
        }

        // If frequency changed, set in sdr
        if (freqHz != 0 && freqHz != (int)lastFreq)
        {
            suppressEvents = true;
            lastFreq = (double)freqHz;
            tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, lastFreq);
            suppressEvents = false;
        }

        // If mode changed, set it in sdr
        if (mode != YaesuMode::MODE_INVALID)
        {
            DemodID modId = DemodID::_RADIO_DEMOD_COUNT;
            switch (mode) 
            {
                case YaesuMode::MODE_AM:
                case YaesuMode::MODE_AMN:
                    modId = RADIO_DEMOD_AM;
                    break;
                case YaesuMode::MODE_CWL:
                case YaesuMode::MODE_CWU:
                    modId = RADIO_DEMOD_CW;
                    break;
                case YaesuMode::MODE_FM:
                case YaesuMode::MODE_FMN:
                    modId = RADIO_DEMOD_NFM;
                    break;
                case YaesuMode::MODE_LSB:
                    modId = RADIO_DEMOD_LSB;
                    break;
                case YaesuMode::MODE_USB:
                    modId = RADIO_DEMOD_USB;
                    break;
                default:
                    modId = DemodID::_RADIO_DEMOD_COUNT;
                    break;
            }

            // If mode is valid and changed, send to SDR to change 
            if (modId != _RADIO_DEMOD_COUNT && mode != lastMode)
            {
                if (RadioModule * radioMod = (RadioModule *)core::moduleManager.getInterface("", "RadioModule"))
                {
                    suppressEvents = true;
                    lastMode = mode;
                    radioMod->selectDemodByID(modId);
                    this->setModeOffset(mode);
                    suppressEvents = false;
                }
            }
        }
    }

    void setModeOffset(YaesuMode mode)
    {
        double mode_offset = 0.0l;
        switch (mode)
        {
            case MODE_AM:
            case MODE_AMN:
                mode_offset = (double)this->am_offset;
                break;
            case MODE_FM:
            case MODE_FMN:
            case MODE_DATAFM:
                mode_offset = (double)this->fm_offset;
                break;
            case MODE_CWU:
            case MODE_CWL:
                mode_offset = (double)this->cw_offset;
                break;
            case MODE_LSB:
                mode_offset = (double)this->lsb_offset;
                break;
            case MODE_USB:
                mode_offset = (double)this->usb_offset;
                break;
            default:
                mode_offset = 0.0l;
                break;
        }

        suppressEvents = true;        
        sigpath::sourceManager.setPanadapterOffset(mode_offset);
        suppressEvents = false;
    }

private:
    static void menuHandler(void* ctx) {
        YaesuCatClientModule* _this = (YaesuCatClientModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (_this->running) { style::beginDisabled(); }
        ImGui::LeftLabel("Comm port");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_yaesucat_comm_port_", _this->name), _this->comm_port, 1023)) {
            config.acquire();
            config.conf[_this->name]["comm_port"] = std::string(_this->comm_port);
            config.release(true);
        }
        //ImGui::SameLine();
        //ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        ImGui::LeftLabel("Baud rate");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##_yaesucat_baud_rate_", _this->name), &_this->baud_rate, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["baud_rate"] = _this->baud_rate;
            config.release(true);
        }
        if (_this->running) { style::endDisabled(); }

        ImGui::LeftLabel("IF Frequency Panadapter");
        ImGui::FillWidth();
        if (ImGui::Checkbox(CONCAT("##_yaesucat_use_if_freq_", _this->name), &_this->useIfTuning)) {
            if (_this->running) {
                if (_this->useIfTuning)
                {
                    sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::PANADAPTER);
                    if (RadioModule * radioMod = (RadioModule *)core::moduleManager.getInterface("", "RadioModule"))
                    {
                        YaesuMode mode = _this->getYaesuMode((DemodID)radioMod->getSelectedDemodId());
                        _this->setModeOffset(mode);
                    }

                }
                else
                {
                    sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::NORMAL);
                    sigpath::sourceManager.setTuningOffset(0.0l);
                }
            }
            config.acquire();
            config.conf[_this->name]["useIfTuning"] = _this->useIfTuning;
            config.release(true);
        }

        if (!_this->useIfTuning)
            ImGui::BeginDisabled();
        ImGui::LeftLabel("IF Frequency");
        ImGui::FillWidth();
        if (ImGui::InputDouble(CONCAT("##_yaesucat_if_freq_", _this->name), &_this->ifFreq, 100.0, 100000.0, "%.0f")) {
            if (_this->running) {
                sigpath::sourceManager.setPanadapterIF(_this->ifFreq);
            }
            config.acquire();
            config.conf[_this->name]["ifFreq"] = _this->ifFreq;
            config.release(true);
        }
        if (!_this->useIfTuning)
            ImGui::EndDisabled();

        if (_this->offset_param_edit) {
            bool valid = false;
            _this->offset_param_edit = _this->offsetParamMenu(valid);

            // If the menu was closed and (TODO) valid, update options
            if (!_this->offset_param_edit && valid) {
                bool currentOffsetChanged = false;
                switch (_this->lastMode) {
                    case MODE_AMN:
                    case MODE_AM:
                        if (_this->am_offset != _this->_am_offset)
                            currentOffsetChanged = true;
                        break;
                    case MODE_CWL:
                    case MODE_CWU:
                        if (_this->cw_offset != _this->_cw_offset)
                            currentOffsetChanged = true;
                        break;
                    case MODE_FMN:
                    case MODE_FM:
                    case MODE_DATAFMN:
                        if (_this->fm_offset != _this->_fm_offset)
                            currentOffsetChanged = true;
                        break;
                    case MODE_LSB:
                        if (_this->lsb_offset != _this->_lsb_offset)
                            currentOffsetChanged = true;
                        break;
                    case MODE_USB:
                        if (_this->usb_offset != _this->_usb_offset)
                            currentOffsetChanged = true;
                        break;
                    default:
                        break;
                }

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

                // If we changed the offset of the current mode, update the live offset
                if (currentOffsetChanged)
                    _this->setModeOffset(_this->lastMode);
            }
        }

        ImGui::FillWidth();
        if (!_this->useIfTuning)
            ImGui::BeginDisabled();
        if (ImGui::Button(CONCAT("Offsets##yaesucat_offset_edit_btn", _this->name), ImVec2(menuWidth, 0))) {
            _this->offset_param_edit = true;
            _this->_fm_offset = _this->fm_offset;
            _this->_am_offset = _this->am_offset;
            _this->_cw_offset = _this->cw_offset;
            _this->_lsb_offset = _this->lsb_offset;
            _this->_usb_offset = _this->usb_offset;
        }
        if (!_this->useIfTuning)
            ImGui::EndDisabled();

        ImGui::FillWidth();
        if (_this->running && ImGui::Button(CONCAT("Stop##_yaesucat_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!_this->running && ImGui::Button(CONCAT("Start##_yaesucat_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->start();
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (_this->running && _this->serial != nullptr)
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Connected");
        else if (_this->running && _this->serial == nullptr)
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Disconnected");
        else
            ImGui::TextUnformatted("Idle");
    }

    bool offsetParamMenu(bool& valid) {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;
        ImGui::OpenPopup("Edit##yaesucat_edit_offset_params_");
        if (ImGui::BeginPopup("Edit##yaesucat_edit_offset_params_", ImGuiWindowFlags_NoResize)) {
            if (ImGui::BeginTable(("yaesucat_offset_param_tbl" + name).c_str(), 2)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("FM");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##yaesucat_offset_fm", &_fm_offset);
                _fm_offset = std::clamp<int>(_fm_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("AM");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##yaesucat_offset_am", &_am_offset);
                _am_offset = std::clamp<int>(_am_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("CW");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##yaesucat_offset_cw", &_cw_offset);
                _cw_offset = std::clamp<int>(_cw_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("LSB");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##yaesucat_offset_lsb", &_lsb_offset);
                _lsb_offset = std::clamp<int>(_lsb_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("USB");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##yaesucat_offset_usb", &_usb_offset);
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
        YaesuCatClientModule* _this = (YaesuCatClientModule*)ctx;
        if (!_this->serial) { return; }
        if (_this->suppressEvents || freq == _this->lastFreq) { return; }
        if (!_this->yaesu_tune(freq)) {
            flog::error("Could not set frequency");
        }
    }

    static void modChangeHandler(DemodID modId, void* ctx) {
        YaesuCatClientModule* _this = (YaesuCatClientModule*)ctx;
        if (!_this->serial) { return; }
        if (_this->suppressEvents) { return; }
        _this->yaesu_setmode(modId);
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    std::recursive_mutex mtx;

    char comm_port[1024];
    int baud_rate = 115200;
    bool suppressEvents = false;

    bool useIfTuning = true;
    double ifFreq = 8830000.0;

    double lastFreq = 0.0l;
    YaesuMode lastMode = YaesuMode::MODE_INVALID;
    YaesuAiMode initial_ai_state = YaesuAiMode::MODE_UNSET;

    bool offset_param_edit = false;
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
    std::vector<uint8_t> serial_buffer;

    EventHandler<double> _retuneHandler;
    EventHandler<DemodID> _modChangeHandler;
    std::chrono::steady_clock::time_point last_serial = std::chrono::steady_clock::now();

    std::shared_ptr<async_comm::Serial> serial = nullptr;
};

MOD_EXPORT void _INIT_() {
    config.setPath(std::string(core::getRoot()) + "/yaesu_cat_client_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new YaesuCatClientModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (YaesuCatClientModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
