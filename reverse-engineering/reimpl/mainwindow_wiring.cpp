// ============================================================================
//  Reconstructed source — MainWindow signal/slot wiring
//  Target : ZDT_Y42_Emm_CAN_Tool V1.2.4  (unpacked from Enigma Protector)
//  Origin : disassembled QObject::connect() call sites in CLEAN_RECONSTRUCTED.exe
//  Note   : sender object names are ui-> placeholders; logic bodies pending decompile.
// ============================================================================
#include "mainwindow.h"
#include "ui_mainwindow.h"

void MainWindow::setupConnections()
{
connect( ui-><sender>, SIGNAL(timeout()), this, SLOT(on_RefreshCanCard_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_pushButtonConnect_clicked()) );
connect( ui-><sender>, SIGNAL(timeout()), this, SLOT(on_SerialReadData()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_ReadSysParameters_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_ReadSysConfs_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_WriteSysConfs_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_WriteSysID_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_ReadSysPID_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_WriteSysPID_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_ReadSysOriginConfs_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_WriteSysOriginConfs_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_SetOrigin_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_trigOrigin_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_exitOrigin_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_enableDriver_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_disableDriver_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_clearCurPosition_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_stopImmediately_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_synMotion_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_resClogging_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_velControl_clicked()) );
connect( ui-><sender>, SIGNAL(clicked()), this, SLOT(on_trajPosControl_clicked()) );
connect( ui-><sender>, SIGNAL(onSetupChanged()), this, SLOT(onSetupChanged()) );
connect( ui-><sender>, SIGNAL(timeout()), this, SLOT(flushQueue()) );
connect( ui-><sender>, SIGNAL(onLogMessage(QDateTime,log_level_t,QString)), this, SLOT(onLogMessage(QDateTime,log_level_t,QString)) );
connect( ui-><sender>, SIGNAL(started()), this, SLOT(run()) );
connect( ui-><sender>, SIGNAL(currentIndexChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(currentIndexChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(currentIndexChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(currentIndexChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(stateChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(stateChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(stateChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(stateChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(stateChanged(int)), this, SLOT(updateUI()) );
connect( ui-><sender>, SIGNAL(onSetupDialogCreated(SetupDialog&)), this, SLOT(onSetupDialogCreated(SetupDialog&)) );
connect( ui-><sender>, SIGNAL(onSetupDialogCreated(SetupDialog&)), this, SLOT(onSetupDialogCreated(SetupDialog&)) );
connect( ui-><sender>, SIGNAL(onSetupDialogCreated(SetupDialog&)), this, SLOT(onSetupDialogCreated(SetupDialog&)) );
}
