// ============================================================================
//  Reconstructed source — mainwindow.h
//  Target : ZDT_Y42_Emm_CAN_Tool V1.2.4  ("张大头闭环伺服" 伺服电机上位机)
//  Origin : reconstructed from Qt5 MOC metadata in CLEAN_RECONSTRUCTED.exe
//  Note   : class/member names recovered from moc string-data + RTTI;
//           signatures are exact, member layout is inferred.
// ============================================================================
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QString>
#include <QDateTime>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

enum log_level_t { log_debug, log_info, log_warning, log_error, log_critical, log_fatal };

// Protocol selectors recovered from the UI string tables
enum ZeroMode   { Nearest, Dir, Senless, Endstop, AbsZero, PowerCut };
enum ZeroDir    { CW, CCW };
enum MotorType  { OPEN, FOC, ESI_RCO, pLR_ESI, ESI_ALO, uLR_ESI };  // 0.9 / 1.8 ...
enum CheckMode  { CHECK_6B /*0x6B*/, CheckXOR, CheckCRC8 };
enum PortMode   { UART, CAN };
enum EnActive   { EnLow /*L*/, EnHigh /*H*/ };

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // --- CAN / serial link ---
    void on_RefreshCanCard_clicked();
    void on_pushButtonConnect_clicked();
    void on_SerialReadyRead();
    void on_SerialReadData();

    // --- system parameters ---
    void on_ReadSysParameters_clicked();
    void on_ReadSysConfs_clicked();
    void on_WriteSysConfs_clicked();
    void on_WriteSysID_clicked();
    void on_ReadSysPID_clicked();
    void on_WriteSysPID_clicked();

    // --- origin / homing (原点回零) ---
    void on_ReadSysOriginConfs_clicked();
    void on_WriteSysOriginConfs_clicked();
    void on_SetOrigin_clicked();        // 设置零点位置
    void on_trigOrigin_clicked();       // 触发回零
    void on_exitOrigin_clicked();       // 强制退出回零

    // --- driver enable/disable (驱动板使能) ---
    void on_enableDriver_clicked();
    void on_disableDriver_clicked();

    // --- motion control (运动控制) ---
    void on_clearCurPosition_clicked(); // 清零位置角度
    void on_stopImmediately_clicked();  // 立即停止
    void on_synMotion_clicked();        // 多机同步运动
    void on_resClogging_clicked();      // 解除堵转保护
    void on_velControl_clicked();       // 速度模式
    void on_trajPosControl_clicked();   // 位置模式(相对/绝对)

    // --- helpers ---
    void flushQueue();
    void run();
    void updateUI();

private:
    Ui::MainWindow *ui;

    QSerialPort *serial;        // openSerialPort / SerialPort
    // CAN link configuration (config.ini [CanConfig])
    bool    canEn;
    int     canChannel;
    QString canDevice;
    int     canBaud;
    int     canAddr;            // 电机地址 / ID
    int     answerTimeOut_CAN;
};

#endif // MAINWINDOW_H
