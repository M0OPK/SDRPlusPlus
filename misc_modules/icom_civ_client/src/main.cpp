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
#include <vector>
#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define CIV_CONTROLLER_ADDRESS 0xe0

SDRPP_MOD_INFO{
    /* Name:            */ "icom_civ_client",
    /* Description:     */ "Client for two way sync with Icom radios",
    /* Author:          */ "M0OPK",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class IcomCivClientModule : public ModuleManager::Instance {

enum IcomMode
{    
    MODE_LSB = 0x00,
    MODE_USB,
    MODE_AM,
    MODE_CW,
    MODE_RTTY,
    MODE_FM,
    MODE_WFM,
    MODE_CWR,
    MODE_RTTYR,
    MODE_INVALID = 0xff
};

public:
    IcomCivClientModule(std::string name) {
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
        if (config.conf[name].contains("civ_address")) {
            civ_address = config.conf[name]["civ_address"];
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

        // Always copy civ address to the text field. Even if it is the default
        snprintf(civ_address_txt, 3, "%02X", civ_address);

        config.release();

        _retuneHandler.ctx = this;
        _retuneHandler.handler = IcomCivClientModule::retuneHandler;
        _modChangeHandler.ctx = this;
        _modChangeHandler.handler = IcomCivClientModule::modChangeHandler;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~IcomCivClientModule() {
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
            serial->register_receive_callback(std::bind(&IcomCivClientModule::civ_callback, this, std::placeholders::_1, std::placeholders::_2));
            if (!serial->init())
            {
                flog::error("Failed to initialize serial port on {0} with baud rate {1}", comm_port, baud_rate);
                return;
            }
        }

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
            IcomMode mode = icom_setmode(modId);
            setModeOffset(mode);
        }

        running = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!running) { return; }

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
            serial.reset();
            serial = nullptr;
        }

        running = false;
    }

    bool icom_tune(double freq)
    {
        if (serial == nullptr)
        {
            flog::error("Serial port not available");
            return false;
        }
        if (lastFreq == freq)
            return true;


        int freqHz = (int)freq;

        // Construct template radio tune command
        uint8_t command[11] = { 0xfe, 0xfe, civ_address, CIV_CONTROLLER_ADDRESS, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd };

        // Here we encode the frequency into BCD and place it into the command array
        for(int currentByte = 5; currentByte < 10; currentByte++)
        {
            uint8_t byteValue = freqHz % 10;
            freqHz /= 10;
            byteValue += (freqHz % 10) * 0x10;
            freqHz /= 10;
            command[currentByte] = byteValue;
        }

        serial->send_bytes(command, 11);
        lastFreq = freq;
        return true;
    }

    IcomMode icom_setmode(DemodID mode)
    {
        if (serial == nullptr)
        {
            flog::error("Serial port not available");
            return MODE_INVALID;
        }

        IcomMode icom_mode = getIcomMode(mode);

        if (icom_mode == MODE_INVALID)
            return MODE_INVALID;

        if (icom_mode == lastMode)
            return icom_mode;

        // Construct/send mode change command
        uint8_t command[7] = { 0xfe, 0xfe, civ_address, CIV_CONTROLLER_ADDRESS, 0x06, (uint8_t)icom_mode, 0xfd };
        serial->send_bytes(command, 7);
        lastMode = icom_mode;
        this->setModeOffset(icom_mode);
        return icom_mode;
    }

    IcomMode getIcomMode(DemodID mode)
    {
        IcomMode icom_mode = MODE_INVALID;
        switch (mode) 
        {
            case RADIO_DEMOD_CW:
                icom_mode = MODE_CW;
                break;
            case RADIO_DEMOD_AM:
                icom_mode = MODE_AM;
                break;
            case RADIO_DEMOD_LSB:
                icom_mode = MODE_LSB;
                break;
            case RADIO_DEMOD_NFM:
                icom_mode = MODE_FM;
                break;
            case RADIO_DEMOD_USB:
                icom_mode = MODE_USB;
                break;
            case RADIO_DEMOD_WFM:
                icom_mode = MODE_WFM;
                break;
            default:
                icom_mode = MODE_INVALID;
                break;
        }
        return icom_mode;
    }

    void civ_callback(const uint8_t* buf, size_t len)
    {
        // Add the waiting data to the buffer
        for (size_t buffer_loc = 0; buffer_loc < len; buffer_loc++)
        {
            serial_buffer.push_back(buf[buffer_loc]);
        }

        // Remove anything prior to the preamble
        while (serial_buffer.size() > 0 && serial_buffer[0] != 0xfe)
        {
            serial_buffer.erase(serial_buffer.begin());
        }

        // If we have nothing left, nothing for now
        if (serial_buffer.size() < 3)
            return;

        // Check there's a valid pre-amble
        if (serial_buffer[0] != 0xfe && serial_buffer[1] != 0xfe)
            return;

        // See if we have an ending
        if (std::find(serial_buffer.begin(), serial_buffer.end(), 0xfd) == serial_buffer.end())
            return;

        // Construct the command and remove bytes from buffer
        std::vector<uint8_t> civ_command;

        bool preamble = true;
        // Take just the actual command data
        while(serial_buffer[0] != 0xfd)
        {
            // Ignore all preambles
            if (serial_buffer[0] != 0xfe || !preamble)
            {
                civ_command.push_back(serial_buffer[0]);
                preamble = false;
            }

            serial_buffer.erase(serial_buffer.begin());
        }

        // Remove final byte (will be 0xfd)
        serial_buffer.erase(serial_buffer.begin());

        // Make sure we have a useful command
        if (civ_command.size() < 5)
            return;

        // Check if this is a transceive command
        if (!(civ_command[0] == 0x00 && civ_command[1] == civ_address && (civ_command[2] == 0x00 || civ_command[2] == 0x01)))
            return;

        // Handle frequency change command
        if (civ_command[2] == 0x00 && civ_command.size() == 8)
        {
            int freqHz = 0;
            for (int pos = 7; pos > 2; pos--)
            {
                uint8_t value = civ_command[pos];
                freqHz *= 10;
                freqHz += (value / 0x10) % 0x10;
                freqHz *= 10;
                freqHz += value % 0x10;
            }
            double freq = (double)freqHz;
            suppressEvents = true;
            lastFreq = freq;
            tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, freq);
            suppressEvents = false;
            //flog::info("Frequency hz {0}", freq);
        }

        // validate length of mode command
        if (civ_command[2] == 0x01 && civ_command.size() == 5)
        {
            IcomMode mode = (IcomMode)civ_command[3];
            DemodID modId = DemodID::_RADIO_DEMOD_COUNT;

            switch (mode) 
            {
                case IcomMode::MODE_AM:
                    modId = RADIO_DEMOD_AM;
                    break;
                case IcomMode::MODE_CW:
                    modId = RADIO_DEMOD_CW;
                    break;
                case IcomMode::MODE_CWR:
                    modId = RADIO_DEMOD_CW;
                    break;
                case IcomMode::MODE_FM:
                    modId = RADIO_DEMOD_NFM;
                    break;
                case IcomMode::MODE_LSB:
                    modId = RADIO_DEMOD_LSB;
                    break;
                case IcomMode::MODE_USB:
                    modId = RADIO_DEMOD_USB;
                    break;
                case IcomMode::MODE_WFM:
                    modId = RADIO_DEMOD_WFM;
                    break;
                default:
                    modId = DemodID::_RADIO_DEMOD_COUNT;
                    break;
            }
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

    void setModeOffset(IcomMode mode)
    {
        double mode_offset = 0.0l;
        switch (mode)
        {
            case MODE_AM:
                mode_offset = (double)this->am_offset;
                break;
            case MODE_FM:
                mode_offset = (double)this->fm_offset;
                break;
            case MODE_CW:
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
        IcomCivClientModule* _this = (IcomCivClientModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (_this->running) { style::beginDisabled(); }
        ImGui::LeftLabel("Comm port");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_icomciv_comm_port_", _this->name), _this->comm_port, 1023)) {
            config.acquire();
            config.conf[_this->name]["comm_port"] = std::string(_this->comm_port);
            config.release(true);
        }
        //ImGui::SameLine();
        //ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        ImGui::LeftLabel("Baud rate");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##_icomciv_baud_rate_", _this->name), &_this->baud_rate, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["baud_rate"] = _this->baud_rate;
            config.release(true);
        }
        ImGui::LeftLabel("CIV Address");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_icomciv_address_", _this->name), _this->civ_address_txt, 3, ImGuiInputTextFlags_CharsHexadecimal)) {
            if (strlen(_this->civ_address_txt) == 0)
                _this->civ_address = 0;
            else
                _this->civ_address = std::stoi(_this->civ_address_txt, 0, 16);
            config.acquire();
            config.conf[_this->name]["civ_address"] = _this->civ_address;
            config.release(true);
        }
        if (_this->running) { style::endDisabled(); }

        ImGui::LeftLabel("IF Frequency Panadapter");
        ImGui::FillWidth();
        if (ImGui::Checkbox(CONCAT("##_icomciv_use_if_freq_", _this->name), &_this->useIfTuning)) {
            if (_this->running) {
                if (_this->useIfTuning)
                {
                    sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::PANADAPTER);
                    if (RadioModule * radioMod = (RadioModule *)core::moduleManager.getInterface("", "RadioModule"))
                    {
                        IcomMode mode = _this->getIcomMode((DemodID)radioMod->getSelectedDemodId());
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
        if (ImGui::InputDouble(CONCAT("##_icomciv_if_freq_", _this->name), &_this->ifFreq, 100.0, 100000.0, "%.0f")) {
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
                    case MODE_AM:
                        if (_this->am_offset != _this->_am_offset)
                            currentOffsetChanged = true;
                        break;
                    case MODE_CW:
                    case MODE_CWR:
                        if (_this->cw_offset != _this->_cw_offset)
                            currentOffsetChanged = true;
                        break;
                    case MODE_FM:
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
        if (ImGui::Button(CONCAT("Offsets##icomciv_offset_edit_btn", _this->name), ImVec2(menuWidth, 0))) {
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
        if (_this->running && ImGui::Button(CONCAT("Stop##_icomciv_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!_this->running && ImGui::Button(CONCAT("Start##_icomciv_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
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
        ImGui::OpenPopup("Edit##icomciv_edit_offset_params_");
        if (ImGui::BeginPopup("Edit##icomciv_edit_offset_params_", ImGuiWindowFlags_NoResize)) {
            if (ImGui::BeginTable(("icomciv_offset_param_tbl" + name).c_str(), 2)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("FM");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##icomciv_offset_fm", &_fm_offset);
                _fm_offset = std::clamp<int>(_fm_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("AM");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##icomciv_offset_am", &_am_offset);
                _am_offset = std::clamp<int>(_am_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("CW");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##icomciv_offset_cw", &_cw_offset);
                _cw_offset = std::clamp<int>(_cw_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("LSB");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##icomciv_offset_lsb", &_lsb_offset);
                _lsb_offset = std::clamp<int>(_lsb_offset, -10000, 10000);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::LeftLabel("USB");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("hz##icomciv_offset_usb", &_usb_offset);
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
        flog::info("Retune handler fired");
        IcomCivClientModule* _this = (IcomCivClientModule*)ctx;
        if (!_this->serial) { return; }
        if (_this->suppressEvents || freq == _this->lastFreq) { return; }
        flog::info("Tuning to {0}", freq);
        if (!_this->icom_tune(freq)) {
            flog::error("Could not set frequency");
        }
    }

    static void modChangeHandler(DemodID modId, void* ctx) {
        IcomCivClientModule* _this = (IcomCivClientModule*)ctx;
        if (!_this->serial) { return; }
        if (_this->suppressEvents) { return; }
        _this->icom_setmode(modId);
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    std::recursive_mutex mtx;

    char comm_port[1024];
    int baud_rate = 115200;
    uint8_t civ_address = 0x88;
    char civ_address_txt[4];
    bool suppressEvents = false;

    bool useIfTuning = true;
    double ifFreq = 8830000.0;

    double lastFreq = 0.0l;
    IcomMode lastMode = IcomMode::MODE_INVALID;

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

    std::shared_ptr<async_comm::Serial> serial = nullptr;
};

MOD_EXPORT void _INIT_() {
    config.setPath(std::string(core::getRoot()) + "/icom_civ_client_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new IcomCivClientModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (IcomCivClientModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
