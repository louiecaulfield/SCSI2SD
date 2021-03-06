//	Copyright (C) 2014 Michael McMaster <michael@codesrc.com>
//
//	This file is part of SCSI2SD.
//
//	SCSI2SD is free software: you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or
//	(at your option) any later version.
//
//	SCSI2SD is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with SCSI2SD.  If not, see <http://www.gnu.org/licenses/>.


// For compilers that support precompilation, includes "wx/wx.h".
#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <wx/app.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/notebook.h>
#include <wx/progdlg.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/windowptr.h>
#include <wx/thread.h>
#include <wx/txtstrm.h>

#include <zipper.hh>

#include "ConfigUtil.hh"
#include "BoardPanel.hh"
#include "TargetPanel.hh"
#include "SCSI2SD_Bootloader.hh"
#include "SCSI2SD_HID.hh"
#include "Firmware.hh"

#include <algorithm>
#include <iomanip>
#include <vector>
#include <set>
#include <sstream>

#if __cplusplus >= 201103L
#include <cstdint>
#include <memory>
using std::shared_ptr;
#else
#include <stdint.h>
#include <tr1/memory>
using std::tr1::shared_ptr;
#endif

#define MIN_FIRMWARE_VERSION 0x0400

using namespace SCSI2SD;

class ProgressWrapper
{
public:
	void setProgressDialog(
		const wxWindowPtr<wxGenericProgressDialog>& dlg,
		size_t maxRows)
	{
		myProgressDialog = dlg;
		myMaxRows = maxRows;
		myNumRows = 0;
	}

	void clearProgressDialog()
	{
		myProgressDialog->Show(false);
		myProgressDialog.reset();
	}

	void update(unsigned char arrayId, unsigned short rowNum)
	{
		if (!myProgressDialog) return;

		myNumRows++;

		std::stringstream ss;
		ss << "Writing flash array " <<
			static_cast<int>(arrayId) << " row " <<
			static_cast<int>(rowNum);
		wxLogMessage("%s", ss.str());
		myProgressDialog->Update(myNumRows, ss.str());
	}

private:
	wxWindowPtr<wxGenericProgressDialog> myProgressDialog;
	size_t myMaxRows;
	size_t myNumRows;
};
static ProgressWrapper TheProgressWrapper;

extern "C"
void ProgressUpdate(unsigned char arrayId, unsigned short rowNum)
{
	TheProgressWrapper.update(arrayId, rowNum);
}

namespace
{

static uint8_t sdCrc7(uint8_t* chr, uint8_t cnt, uint8_t crc)
{
	uint8_t a;
	for(a = 0; a < cnt; a++)
	{
		uint8_t data = chr[a];
		uint8_t i;
		for(i = 0; i < 8; i++)
		{
			crc <<= 1;
			if ((data & 0x80) ^ (crc & 0x80))
			{
				crc ^= 0x09;
			}
			data <<= 1;
		}
	}
	return crc & 0x7F;
}

class TimerLock
{
public:
	TimerLock(wxTimer* timer) :
		myTimer(timer),
		myInterval(myTimer->GetInterval())
	{
		myTimer->Stop();
	};

	virtual ~TimerLock()
	{
		if (myTimer && myInterval > 0)
		{
			myTimer->Start(myInterval);
		}
	}
private:
	wxTimer* myTimer;
	int myInterval;
};

class AppFrame : public wxFrame
{
public:
	AppFrame() :
		wxFrame(NULL, wxID_ANY, "scsi2sd-util", wxPoint(50, 50), wxSize(600, 650)),
		myInitialConfig(false),
		myTickCounter(0),
		myLastPollTime(0)
	{
		wxMenu *menuFile = new wxMenu();
		menuFile->Append(
			ID_SaveFile,
			_("&Save to file..."),
			_("Save settings to local file."));
		menuFile->Append(
			ID_OpenFile,
			_("&Open file..."),
			_("Load settings from local file."));
		menuFile->AppendSeparator();
		menuFile->Append(
			ID_ConfigDefaults,
			_("Load &Defaults"),
			_("Load default configuration options."));
		menuFile->Append(
			ID_Firmware,
			_("&Upgrade Firmware..."),
			_("Upgrade or inspect device firmware version."));
		menuFile->Append(
			ID_Bootloader,
			_("&Upgrade Bootloader (ADVANCED) ..."),
			_("Upgrade device bootloader."));
		menuFile->AppendSeparator();
		menuFile->Append(wxID_EXIT);

		wxMenu *menuWindow= new wxMenu();
		menuWindow->Append(
			ID_LogWindow,
			_("Show &Log"),
			_("Show debug log window"));

		wxMenu *menuDebug = new wxMenu();
		mySCSILogChk = menuDebug->AppendCheckItem(
			ID_SCSILog,
			_("Log SCSI data"),
			_("Log SCSI commands"));

		mySelfTestChk = menuDebug->AppendCheckItem(
			ID_SelfTest,
			_("SCSI Standalone Self-Test"),
			_("SCSI Standalone Self-Test"));

		wxMenu *menuHelp = new wxMenu();
		menuHelp->Append(wxID_ABOUT);

		wxMenuBar *menuBar = new wxMenuBar();
		menuBar->Append( menuFile, _("&File") );
		menuBar->Append( menuDebug, _("&Debug") );
		menuBar->Append( menuWindow, _("&Window") );
		menuBar->Append( menuHelp, _("&Help") );
		SetMenuBar( menuBar );

		CreateStatusBar();

		{
			wxPanel* cfgPanel = new wxPanel(this);
			wxFlexGridSizer *fgs = new wxFlexGridSizer(3, 1, 15, 15);
			cfgPanel->SetSizer(fgs);

			// Empty space below menu bar.
			fgs->Add(5, 5, wxALL);

			wxNotebook* tabs = new wxNotebook(cfgPanel, ID_Notebook);
			myBoardPanel = new BoardPanel(tabs, ConfigUtil::DefaultBoardConfig());
			tabs->AddPage(myBoardPanel, _("General Settings"));
			for (int i = 0; i < MAX_SCSI_TARGETS; ++i)
			{
				TargetPanel* target =
					new TargetPanel(tabs, ConfigUtil::Default(i));
				myTargets.push_back(target);
				std::stringstream ss;
				ss << "Device " << (i + 1);
				tabs->AddPage(target, ss.str());
				target->Fit();
			}
			tabs->Fit();
			fgs->Add(tabs);


			wxPanel* btnPanel = new wxPanel(cfgPanel);
			wxFlexGridSizer *btnFgs = new wxFlexGridSizer(1, 2, 5, 5);
			btnPanel->SetSizer(btnFgs);
			myLoadButton =
				new wxButton(btnPanel, ID_BtnLoad, _("Load from device"));
			btnFgs->Add(myLoadButton);
			mySaveButton =
				new wxButton(btnPanel, ID_BtnSave, _("Save to device"));
			btnFgs->Add(mySaveButton);
			fgs->Add(btnPanel);

			btnPanel->Fit();
			cfgPanel->Fit();
		}
		//Fit(); // Needed to reduce window size on Windows
		FitInside(); // Needed on Linux to prevent status bar overlap

		myLogWindow = new wxLogWindow(this, _("scsi2sd-util debug log"), true);
		myLogWindow->PassMessages(false); // Prevent messagebox popups

		myTimer = new wxTimer(this, ID_Timer);
		myTimer->Start(16); //ms, suitable for scsi debug logging
	}

private:
	wxLogWindow* myLogWindow;
	BoardPanel* myBoardPanel;
	std::vector<TargetPanel*> myTargets;
	wxButton* myLoadButton;
	wxButton* mySaveButton;
	wxMenuItem* mySCSILogChk;
	wxMenuItem* mySelfTestChk;
	wxTimer* myTimer;
	shared_ptr<HID> myHID;
	shared_ptr<Bootloader> myBootloader;
	bool myInitialConfig;

	uint8_t myTickCounter;

	time_t myLastPollTime;

	void mmLogStatus(const std::string& msg)
	{
		// We set PassMessages to false on our log window to prevent popups, but
		// this also prevents wxLogStatus from updating the status bar.
		SetStatusText(msg);
		wxLogMessage(this, "%s", msg.c_str());
	}

	void onConfigChanged(wxCommandEvent& event)
	{
		evaluate();
	}

	void evaluate()
	{
		bool valid = true;

		// Check for duplicate SCSI IDs
		std::set<uint8_t> enabledID;

		// Check for overlapping SD sectors.
		std::vector<std::pair<uint32_t, uint64_t> > sdSectors;

		bool isTargetEnabled = false; // Need at least one enabled
		uint32_t autoStartSector = 0;
		for (size_t i = 0; i < myTargets.size(); ++i)
		{
			myTargets[i]->setAutoStartSector(autoStartSector);
			valid = myTargets[i]->evaluate() && valid;

			if (myTargets[i]->isEnabled())
			{
				isTargetEnabled = true;
				uint8_t scsiID = myTargets[i]->getSCSIId();
				if (enabledID.find(scsiID) != enabledID.end())
				{
					myTargets[i]->setDuplicateID(true);
					valid = false;
				}
				else
				{
					enabledID.insert(scsiID);
					myTargets[i]->setDuplicateID(false);
				}

				auto sdSectorRange = myTargets[i]->getSDSectorRange();
				for (auto it(sdSectors.begin()); it != sdSectors.end(); ++it)
				{
					if (sdSectorRange.first < it->second &&
						sdSectorRange.second > it->first)
					{
						valid = false;
						myTargets[i]->setSDSectorOverlap(true);
					}
					else
					{
						myTargets[i]->setSDSectorOverlap(false);
					}
				}
				sdSectors.push_back(sdSectorRange);
				autoStartSector = sdSectorRange.second;
			}
			else
			{
				myTargets[i]->setDuplicateID(false);
				myTargets[i]->setSDSectorOverlap(false);
			}
		}

		valid = valid && isTargetEnabled; // Need at least one.

		mySaveButton->Enable(
			valid &&
			myHID &&
			(myHID->getFirmwareVersion() >= MIN_FIRMWARE_VERSION));

		myLoadButton->Enable(
			myHID &&
			(myHID->getFirmwareVersion() >= MIN_FIRMWARE_VERSION));
	}


	enum
	{
		ID_ConfigDefaults = wxID_HIGHEST + 1,
		ID_Firmware,
		ID_Bootloader,
		ID_Timer,
		ID_Notebook,
		ID_BtnLoad,
		ID_BtnSave,
		ID_LogWindow,
		ID_SCSILog,
		ID_SelfTest,
		ID_SaveFile,
		ID_OpenFile
	};

	void OnID_ConfigDefaults(wxCommandEvent& event)
	{
		myBoardPanel->setConfig(ConfigUtil::DefaultBoardConfig());
		for (size_t i = 0; i < myTargets.size(); ++i)
		{
			myTargets[i]->setConfig(ConfigUtil::Default(i));
		}
	}

	void OnID_SaveFile(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);



		wxFileDialog dlg(
			this,
			"Save config settings",
			"",
			"",
			"XML files (*.xml)|*.xml",
			wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
		if (dlg.ShowModal() == wxID_CANCEL) return;

		wxFileOutputStream file(dlg.GetPath());
		if (!file.IsOk())
		{
			wxLogError("Cannot save settings to file '%s'.", dlg.GetPath());
			return;
		}

		wxTextOutputStream s(file);

		s << "<SCSI2SD>\n";

		s << ConfigUtil::toXML(myBoardPanel->getConfig());
		for (size_t i = 0; i < myTargets.size(); ++i)
		{
			s << ConfigUtil::toXML(myTargets[i]->getConfig());
		}

		s << "</SCSI2SD>\n";
	}

	void OnID_OpenFile(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);

		wxFileDialog dlg(
			this,
			"Load config settings",
			"",
			"",
			"XML files (*.xml)|*.xml",
			wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (dlg.ShowModal() == wxID_CANCEL) return;

		try
		{
			std::pair<BoardConfig, std::vector<TargetConfig>> configs(
				ConfigUtil::fromXML(std::string(dlg.GetPath())));

			myBoardPanel->setConfig(configs.first);

			size_t i;
			for (i = 0; i < configs.second.size() && i < myTargets.size(); ++i)
			{
				myTargets[i]->setConfig(configs.second[i]);
			}

			for (; i < myTargets.size(); ++i)
			{
				myTargets[i]->setConfig(ConfigUtil::Default(i));
			}
		}
		catch (std::exception& e)
		{
			wxLogError(
				"Cannot load settings from file '%s'.\n%s",
				dlg.GetPath(),
				e.what());

			wxMessageBox(
				e.what(),
				"Load error",
				wxOK | wxICON_ERROR);
		}
	}

	void OnID_Firmware(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);
		doFirmwareUpdate();
	}

	void OnID_Bootloader(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);
		doBootloaderUpdate();
	}

	void OnID_LogWindow(wxCommandEvent& event)
	{
		myLogWindow->Show();
	}

	void doFirmwareUpdate()
	{
		wxFileDialog dlg(
			this,
			"Load firmware file",
			"",
			"",
			"SCSI2SD Firmware files (*.scsi2sd;*.cyacd)|*.cyacd;*.scsi2sd",
			wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (dlg.ShowModal() == wxID_CANCEL) return;

		std::string filename(dlg.GetPath());

		wxWindowPtr<wxGenericProgressDialog> progress(
			new wxGenericProgressDialog(
				"Searching for bootloader",
				"Searching for bootloader",
				100,
				this,
				wxPD_AUTO_HIDE | wxPD_CAN_ABORT)
				);
		mmLogStatus("Searching for bootloader");
		while (true)
		{
			try
			{
				if (!myHID) myHID.reset(HID::Open());
				if (myHID)
				{
					mmLogStatus("Resetting SCSI2SD into bootloader");

					myHID->enterBootloader();
					myHID.reset();
				}


				if (!myBootloader)
				{
					myBootloader.reset(Bootloader::Open());
					if (myBootloader)
					{
						mmLogStatus("Bootloader found");
						break;
					}
				}

				else if (myBootloader)
				{
					// Verify the USB HID connection is valid
					if (!myBootloader->ping())
					{
						mmLogStatus("Bootloader ping failed");
						myBootloader.reset();
					}
					else
					{
						mmLogStatus("Bootloader found");
						break;
					}
				}
			}
			catch (std::exception& e)
			{
				mmLogStatus(e.what());
				myHID.reset();
				myBootloader.reset();
			}
			wxMilliSleep(100);
			if (!progress->Pulse())
			{
				return; // user cancelled.
			}
		}

		int totalFlashRows = 0;
		std::string tmpFile;
		try
		{
			zipper::ReaderPtr reader(new zipper::FileReader(filename));
			zipper::Decompressor decomp(reader);
			std::vector<zipper::CompressedFilePtr> files(decomp.getEntries());
			for (auto it(files.begin()); it != files.end(); it++)
			{
				if (myBootloader->isCorrectFirmware((*it)->getPath()))
				{
					std::stringstream msg;
					msg << "Found firmware entry " << (*it)->getPath() <<
						" within archive " << filename;
					mmLogStatus(msg.str());
					tmpFile =
						wxFileName::CreateTempFileName(
							_("SCSI2SD_Firmware"), static_cast<wxFile*>(NULL)
							);
					zipper::FileWriter out(tmpFile);
					(*it)->decompress(out);
					msg.clear();
					msg << "Firmware extracted to " << tmpFile;
					mmLogStatus(msg.str());
					break;
				}
			}

			if (tmpFile.empty())
			{
				// TODO allow "force" option
				wxMessageBox(
					"Wrong filename",
					"Wrong filename",
					wxOK | wxICON_ERROR);
				return;
			}

			Firmware firmware(tmpFile);
			totalFlashRows = firmware.totalFlashRows();
		}
		catch (std::exception& e)
		{
			mmLogStatus(e.what());
			std::stringstream msg;
			msg << "Could not open firmware file: " << e.what();
			wxMessageBox(
				msg.str(),
				"Bad file",
				wxOK | wxICON_ERROR);
			wxRemoveFile(tmpFile);
			return;
		}

		{
			wxWindowPtr<wxGenericProgressDialog> progress(
				new wxGenericProgressDialog(
					"Loading firmware",
					"Loading firmware",
					totalFlashRows,
					this,
					wxPD_AUTO_HIDE | wxPD_REMAINING_TIME)
					);
			TheProgressWrapper.setProgressDialog(progress, totalFlashRows);
		}

		std::stringstream msg;
		msg << "Upgrading firmware from file: " << tmpFile;
		mmLogStatus(msg.str());

		try
		{
			myBootloader->load(tmpFile, &ProgressUpdate);
			TheProgressWrapper.clearProgressDialog();

			wxMessageBox(
				"Firmware update successful",
				"Firmware OK",
				wxOK);
			mmLogStatus("Firmware update successful");


			myHID.reset();
			myBootloader.reset();
		}
		catch (std::exception& e)
		{
			TheProgressWrapper.clearProgressDialog();
			mmLogStatus(e.what());
			myHID.reset();
			myBootloader.reset();

			wxMessageBox(
				"Firmware Update Failed",
				e.what(),
				wxOK | wxICON_ERROR);

			wxRemoveFile(tmpFile);
		}
	}

	void doBootloaderUpdate()
	{
		if (!myHID)
		{
			wxMessageBox(
				"No device",
				"No device",
				wxOK | wxICON_ERROR);
			return;
		}

		wxFileDialog dlg(
			this,
			"Load bootloader file",
			"",
			"",
			"SCSI2SD Bootloader files (*.bin)|*.bin",
			wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (dlg.ShowModal() == wxID_CANCEL) return;

		std::string filename(dlg.GetPath());

		wxFile file(filename);
		if (file.Length() != 0x2400)
		{
			wxMessageBox(
				"Invalid file",
				"Invalid file",
				wxOK | wxICON_ERROR);
			return;

		}
		uint8_t data[0x2400];
		if (file.Read(data, sizeof(data)) != sizeof(data))
		{
			wxMessageBox(
				"Couldn't read file",
				"Couldn't read file",
				wxOK | wxICON_ERROR);
			return;
		}

		static char magic[] = {
			'P', 0, 'S', 0, 'o', 0, 'C', 0, '3', 0, ' ', 0,
			'B', 0, 'o', 0, 'o', 0, 't', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0};

		uint8_t* dataEnd = data + sizeof(data);
		if (std::search(data, dataEnd, magic, magic + sizeof(magic)) >= dataEnd)
		{
			wxMessageBox(
				"Bad file",
				"Not a valid bootloader file.",
				wxOK | wxICON_ERROR);
			return;
		}

		std::stringstream msg;
		msg << "Upgrading bootloader from file: " << filename;
		mmLogStatus(msg.str());

		wxWindowPtr<wxGenericProgressDialog> progress(
			new wxGenericProgressDialog(
				"Update bootloader",
				"Update bootloader",
				100,
				this,
				wxPD_REMAINING_TIME)
				);


		int currentProgress = 0;
		int totalProgress = 36;

		for (size_t flashRow = 0; flashRow < 36; ++flashRow)
		{
			std::stringstream ss;
			ss << "Programming flash array 0 row " << (flashRow);
			mmLogStatus(ss.str());
			currentProgress += 1;

			if (currentProgress == totalProgress)
			{
				ss.str("Save Complete.");
				mmLogStatus("Save Complete.");
			}
			if (!progress->Update(
					(100 * currentProgress) / totalProgress,
					ss.str()
					)
				)
			{
				goto abort;
			}

			uint8_t* rowData = data + (flashRow * 256);
			std::vector<uint8_t> flashData(rowData, rowData + 256);
			try
			{
				myHID->writeFlashRow(0, flashRow, flashData);
			}
			catch (std::runtime_error& e)
			{
				mmLogStatus(e.what());
				goto err;
			}
		}

		goto out;

	err:
		mmLogStatus("Bootloader update failed");
		progress->Update(100, "Bootloader update failed");
		goto out;

	abort:
		mmLogStatus("Bootloader update aborted");

	out:
		return;


	}

	void dumpSCSICommand(std::vector<uint8_t> buf)
        {
		std::stringstream msg;
		msg << std::hex;
		for (size_t i = 0; i < 32 && i < buf.size(); ++i)
		{
			msg << std::setfill('0') << std::setw(2) <<
			static_cast<int>(buf[i]) << ' ';
		}
		wxLogMessage(this, msg.str().c_str());
        }

	void logSCSI()
	{
		if (!mySCSILogChk->IsChecked() ||
			!myHID)
		{
			return;
		}
		try
		{
			std::vector<uint8_t> info(HID::HID_PACKET_SIZE);
			if (myHID->readSCSIDebugInfo(info))
			{
				dumpSCSICommand(info);
			}
		}
		catch (std::exception& e)
		{
			wxLogWarning(this, e.what());
			myHID.reset();
		}
	}

	void OnID_Timer(wxTimerEvent& event)
	{
		logSCSI();
		time_t now = time(NULL);
		if (now == myLastPollTime) return;
		myLastPollTime = now;

		// Check if we are connected to the HID device.
		// AND/or bootloader device.
		try
		{
			if (myBootloader)
			{
				// Verify the USB HID connection is valid
				if (!myBootloader->ping())
				{
					myBootloader.reset();
				}
			}

			if (!myBootloader)
			{
				myBootloader.reset(Bootloader::Open());

				if (myBootloader)
				{
					mmLogStatus("SCSI2SD Bootloader Ready");
				}
			}

			int supressLog = 0;
			if (myHID && myHID->getFirmwareVersion() < MIN_FIRMWARE_VERSION)
			{
				// No method to check connection is still valid.
				// So assume it isn't.
				myHID.reset();
				supressLog = 1;
			}
			else if (myHID && !myHID->ping())
			{
				// Verify the USB HID connection is valid
				myHID.reset();
			}

			if (!myHID)
			{
				myHID.reset(HID::Open());
				if (myHID)
				{
					if (myHID->getFirmwareVersion() < MIN_FIRMWARE_VERSION)
					{
						if (!supressLog)
						{
							// Oh dear, old firmware
							std::stringstream msg;
							msg << "Firmware update required. Version " <<
								myHID->getFirmwareVersionStr();
							mmLogStatus(msg.str());
						}
					}
					else
					{
						std::stringstream msg;
						msg << "SCSI2SD Ready, firmware version " <<
							myHID->getFirmwareVersionStr();
						mmLogStatus(msg.str());

						std::vector<uint8_t> csd(myHID->getSD_CSD());
						std::vector<uint8_t> cid(myHID->getSD_CID());
						std::stringstream sdinfo;
						sdinfo << "SD Capacity (512-byte sectors): " <<
							myHID->getSDCapacity() << std::endl;

						sdinfo << "SD CSD Register: ";
						if (sdCrc7(&csd[0], 15, 0) != (csd[15] >> 1))
						{
							sdinfo << "BADCRC ";
						}
						for (size_t i = 0; i < csd.size(); ++i)
						{
							sdinfo <<
								std::hex << std::setfill('0') << std::setw(2) <<
								static_cast<int>(csd[i]);
						}
						sdinfo << std::endl;
						sdinfo << "SD CID Register: ";
						if (sdCrc7(&cid[0], 15, 0) != (cid[15] >> 1))
						{
							sdinfo << "BADCRC ";
						}
						for (size_t i = 0; i < cid.size(); ++i)
						{
							sdinfo <<
								std::hex << std::setfill('0') << std::setw(2) <<
								static_cast<int>(cid[i]);
						}

						wxLogMessage(this, "%s", sdinfo.str());

						if (mySelfTestChk->IsChecked())
						{
							std::stringstream scsiInfo;
							scsiInfo << "SCSI Self-Test: " <<
								(myHID->scsiSelfTest() ? "Passed" : "FAIL");
							wxLogMessage(this, "%s", scsiInfo.str());
						}

						if (!myInitialConfig)
						{
/* This doesn't work properly, and causes crashes.
							wxCommandEvent loadEvent(wxEVT_NULL, ID_BtnLoad);
							GetEventHandler()->AddPendingEvent(loadEvent);
*/
						}

					}
				}
				else
				{
					char ticks[] = {'/', '-', '\\', '|'};
					std::stringstream ss;
					ss << "Searching for SCSI2SD device " << ticks[myTickCounter % sizeof(ticks)];
					myTickCounter++;
					SetStatusText(ss.str());
				}
			}
		}
		catch (std::runtime_error& e)
		{
			std::cerr << e.what() << std::endl;
			mmLogStatus(e.what());
		}

		evaluate();
	}

	void doLoad(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);
		if (!myHID) return;

		mmLogStatus("Loading configuration");

		wxWindowPtr<wxGenericProgressDialog> progress(
			new wxGenericProgressDialog(
				"Load config settings",
				"Loading config settings",
				100,
				this,
				wxPD_CAN_ABORT | wxPD_REMAINING_TIME)
				);

		int currentProgress = 0;
		int totalProgress = myTargets.size() * SCSI_CONFIG_ROWS + 1;

		// Read board config first.
		std::vector<uint8_t> boardCfgFlashData;
		int flashRow = SCSI_CONFIG_BOARD_ROW;
		{
			std::stringstream ss;
			ss << "Reading flash array " << SCSI_CONFIG_ARRAY <<
				" row " << flashRow;
			mmLogStatus(ss.str());
			currentProgress += 1;

			if (!progress->Update(
					(100 * currentProgress) / totalProgress,
					ss.str()
					)
				)
			{
				goto abort;
			}

			try
			{
				myHID->readFlashRow(
					SCSI_CONFIG_ARRAY, flashRow, boardCfgFlashData);
				myBoardPanel->setConfig(
					ConfigUtil::boardConfigFromBytes(&boardCfgFlashData[0]));
			}
			catch (std::runtime_error& e)
			{
				mmLogStatus(e.what());
				goto err;
			}
		}

		flashRow = SCSI_CONFIG_0_ROW;
		for (size_t i = 0;
			i < myTargets.size();
			++i, flashRow += SCSI_CONFIG_ROWS)
		{
			std::vector<uint8_t> raw(sizeof(TargetConfig));

			for (size_t j = 0; j < SCSI_CONFIG_ROWS; ++j)
			{
				std::stringstream ss;
				ss << "Reading flash array " << SCSI_CONFIG_ARRAY <<
					" row " << (flashRow + j);
				mmLogStatus(ss.str());
				currentProgress += 1;
				if (currentProgress == totalProgress)
				{
					ss.str("Load Complete.");
					mmLogStatus("Load Complete.");
				}

				if (!progress->Update(
						(100 * currentProgress) / totalProgress,
						ss.str()
						)
					)
				{
					goto abort;
				}

				std::vector<uint8_t> flashData;

				try
				{
					myHID->readFlashRow(
						SCSI_CONFIG_ARRAY, flashRow + j, flashData);

				}
				catch (std::runtime_error& e)
				{
					mmLogStatus(e.what());
					goto err;
				}

				std::copy(
					flashData.begin(),
					flashData.end(),
					&raw[j * SCSI_CONFIG_ROW_SIZE]);
			}
			myTargets[i]->setConfig(ConfigUtil::fromBytes(&raw[0]));
		}

		// Support old boards without board config set
		if (memcmp(&boardCfgFlashData[0], "BCFG", 4)) {
			BoardConfig defCfg = ConfigUtil::DefaultBoardConfig();
			defCfg.flags = myTargets[0]->getConfig().flagsDEPRECATED;
			myBoardPanel->setConfig(defCfg);
		}

		myInitialConfig = true;
		goto out;

	err:
		mmLogStatus("Load failed");
		progress->Update(100, "Load failed");
		goto out;

	abort:
		mmLogStatus("Load Aborted");

	out:
		return;
	}

	void doSave(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);
		if (!myHID) return;

		mmLogStatus("Saving configuration");

		wxWindowPtr<wxGenericProgressDialog> progress(
			new wxGenericProgressDialog(
				"Save config settings",
				"Saving config settings",
				100,
				this,
				wxPD_CAN_ABORT | wxPD_REMAINING_TIME)
				);


		int currentProgress = 0;
		int totalProgress = myTargets.size() * SCSI_CONFIG_ROWS + 1;

		// Write board config first.
		int flashRow = SCSI_CONFIG_BOARD_ROW;
		{
			std::stringstream ss;
			ss << "Programming flash array " << SCSI_CONFIG_ARRAY <<
				" row " << flashRow;
			mmLogStatus(ss.str());
			currentProgress += 1;

			if (!progress->Update(
					(100 * currentProgress) / totalProgress,
					ss.str()
					)
				)
			{
				goto abort;
			}

			std::vector<uint8_t> flashData =
				ConfigUtil::boardConfigToBytes(myBoardPanel->getConfig());
			try
			{
				myHID->writeFlashRow(
					SCSI_CONFIG_ARRAY, flashRow, flashData);
			}
			catch (std::runtime_error& e)
			{
				mmLogStatus(e.what());
				goto err;
			}
		}

		flashRow = SCSI_CONFIG_0_ROW;
		for (size_t i = 0;
			i < myTargets.size();
			++i, flashRow += SCSI_CONFIG_ROWS)
		{
			TargetConfig config(myTargets[i]->getConfig());
			std::vector<uint8_t> raw(ConfigUtil::toBytes(config));

			for (size_t j = 0; j < SCSI_CONFIG_ROWS; ++j)
			{
				std::stringstream ss;
				ss << "Programming flash array " << SCSI_CONFIG_ARRAY <<
					" row " << (flashRow + j);
				mmLogStatus(ss.str());
				currentProgress += 1;

				if (currentProgress == totalProgress)
				{
					ss.str("Save Complete.");
					mmLogStatus("Save Complete.");
				}
				if (!progress->Update(
						(100 * currentProgress) / totalProgress,
						ss.str()
						)
					)
				{
					goto abort;
				}

				std::vector<uint8_t> flashData(SCSI_CONFIG_ROW_SIZE, 0);
				std::copy(
					&raw[j * SCSI_CONFIG_ROW_SIZE],
					&raw[(1+j) * SCSI_CONFIG_ROW_SIZE],
					flashData.begin());
				try
				{
					myHID->writeFlashRow(
						SCSI_CONFIG_ARRAY, flashRow + j, flashData);
				}
				catch (std::runtime_error& e)
				{
					mmLogStatus(e.what());
					goto err;
				}
			}
		}

		myHID.reset();

		goto out;

	err:
		mmLogStatus("Save failed");
		progress->Update(100, "Save failed");
		goto out;

	abort:
		mmLogStatus("Save Aborted");

	out:
		return;
	}

	// Note: Don't confuse this with the wxApp::OnExit virtual method
	void OnExitEvt(wxCommandEvent& event);

	void OnCloseEvt(wxCloseEvent& event);

	void OnAbout(wxCommandEvent& event)
	{
		wxMessageBox(
			"SCSI2SD (scsi2sd-util)\n"
			"Copyright (C) 2014 Michael McMaster <michael@codesrc.com>\n"
			"\n"
"This program is free software: you can redistribute it and/or modify\n"
"it under the terms of the GNU General Public License as published by\n"
"the Free Software Foundation, either version 3 of the License, or\n"
"(at your option) any later version.\n"
"\n"
"This program is distributed in the hope that it will be useful,\n"
"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
"GNU General Public License for more details.\n"
"\n"
"You should have received a copy of the GNU General Public License\n"
"along with this program.  If not, see <http://www.gnu.org/licenses/>.\n",

			"About scsi2sd-util", wxOK | wxICON_INFORMATION );
	}

	wxDECLARE_EVENT_TABLE();
};

wxBEGIN_EVENT_TABLE(AppFrame, wxFrame)
	EVT_MENU(AppFrame::ID_ConfigDefaults, AppFrame::OnID_ConfigDefaults)
	EVT_MENU(AppFrame::ID_Firmware, AppFrame::OnID_Firmware)
	EVT_MENU(AppFrame::ID_Bootloader, AppFrame::OnID_Bootloader)
	EVT_MENU(AppFrame::ID_LogWindow, AppFrame::OnID_LogWindow)
	EVT_MENU(AppFrame::ID_SaveFile, AppFrame::OnID_SaveFile)
	EVT_MENU(AppFrame::ID_OpenFile, AppFrame::OnID_OpenFile)
	EVT_MENU(wxID_EXIT, AppFrame::OnExitEvt)
	EVT_MENU(wxID_ABOUT, AppFrame::OnAbout)

	EVT_TIMER(AppFrame::ID_Timer, AppFrame::OnID_Timer)

	EVT_COMMAND(wxID_ANY, ConfigChangedEvent, AppFrame::onConfigChanged)

	EVT_BUTTON(ID_BtnSave, AppFrame::doSave)
	EVT_BUTTON(ID_BtnLoad, AppFrame::doLoad)

	EVT_CLOSE(AppFrame::OnCloseEvt)

wxEND_EVENT_TABLE()



class App : public wxApp
{
public:
	virtual bool OnInit()
	{
		AppFrame* frame = new AppFrame();
		frame->Show(true);
		SetTopWindow(frame);
		return true;
	}
};
} // namespace

// Main Method
wxIMPLEMENT_APP(App);

void
AppFrame::OnExitEvt(wxCommandEvent& event)
{
	wxGetApp().ExitMainLoop();
}

void
AppFrame::OnCloseEvt(wxCloseEvent& event)
{
	wxGetApp().ExitMainLoop();
}

