#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H

#include <QDialog>
#include "settings.h"

class Settings;
class QTabWidget;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QPushButton;
class QCheckBox;
class QListWidget;
class QTableWidget;

// IDM 风格多标签页设置对话框：常规 / 文件类型 / 下载 / 连接 / 代理 / 站点登录
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(Settings& settings, QWidget* parent = nullptr);

    // 切到指定标签页（0=常规 … 5=站点登录）。走查工具用它把每个标签页都截下来，
    // 否则截图工具只能看到默认的「常规」页，改在别页的文案永远验证不到。
    void selectTab(int index);

private slots:
    void onBrowseDir();
    void onAddExt();
    void onRemoveExt();
    void onAddSite();
    void onEditSite();
    void onRemoveSite();
    void onDownloadYtDlpClicked();
    void onBrowseFfmpegClicked();
    void onDownloadAria2Clicked();
    void onRegenerateTokenClicked();
    void accept() override;

signals:
    void downloadYtDlpRequested();
    void downloadAria2Requested();

private:
    void buildGeneralTab(QWidget* page);
    void buildFileTypesTab(QWidget* page);
    void buildDownloadsTab(QWidget* page);
    void buildConnectionTab(QWidget* page);
    void buildProxyTab(QWidget* page);
    void buildSitesTab(QWidget* page);
    void buildWebTab(QWidget* page);
    void refreshSiteTable();

    Settings& m_settings;

    // 常规
    QLineEdit* m_dirEdit        = nullptr;
    QComboBox* m_themeCombo     = nullptr;
    QCheckBox* m_closeTrayCheck = nullptr;
    QCheckBox* m_clipboardCheck = nullptr;

    // 文件类型
    QListWidget* m_extList = nullptr;
    QLineEdit*    m_extEdit = nullptr;

    // 下载
    QSpinBox*  m_threadsSpin    = nullptr;
    QSpinBox*  m_concurrentSpin = nullptr;
    QSpinBox*  m_limitSpin      = nullptr;
    int        m_limitAtOpen    = 0;    // 打开时的限速值，用于判断用户是否真的改过
    QSpinBox*  m_connSpin       = nullptr;
    QSpinBox*  m_retrySpin      = nullptr;
    QLineEdit* m_uaEdit         = nullptr;
    QCheckBox* m_autoArchiveCheck = nullptr;
    QCheckBox* m_autoYtDlpCheck   = nullptr;
    QLineEdit* m_ytDlpPathEdit    = nullptr;
    QPushButton* m_ytDlpBtn       = nullptr;
    QLineEdit* m_ffmpegPathEdit   = nullptr;
    QPushButton* m_ffmpegBtn      = nullptr;
    QCheckBox* m_autoAria2Check   = nullptr;
    QCheckBox* m_autoFfmpegCheck  = nullptr;
    QLineEdit* m_aria2PathEdit    = nullptr;
    QPushButton* m_aria2Btn       = nullptr;
    QComboBox* m_powerCombo       = nullptr;
    QSpinBox*  m_graceSpin        = nullptr;

    // 远程 / Web
    QCheckBox* m_webCheck    = nullptr;
    QSpinBox*  m_webPortSpin = nullptr;
    QLineEdit* m_webTokenEdit = nullptr;
    QPushButton* m_webTokenBtn = nullptr;

    // 连接
    QSpinBox*  m_timeoutSpin = nullptr;
    QCheckBox* m_http2Check  = nullptr;

    // 代理
    QComboBox* m_proxyTypeCombo = nullptr;
    QLineEdit* m_proxyHostEdit  = nullptr;
    QSpinBox*  m_proxyPortSpin  = nullptr;
    QLineEdit* m_proxyUserEdit  = nullptr;
    QLineEdit* m_proxyPassEdit  = nullptr;

    // 站点登录
    QTableWidget* m_siteTable = nullptr;
};

#endif // SETTINGS_DIALOG_H
