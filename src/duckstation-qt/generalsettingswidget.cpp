#include "generalsettingswidget.h"
#include "autoupdaterdialog.h"
#include "frontend-common/controller_interface.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

GeneralSettingsWidget::GeneralSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(ControllerInterface::Backend::Count); i++)
  {
    m_ui.controllerBackend->addItem(qApp->translate(
      "ControllerInterface", ControllerInterface::GetBackendName(static_cast<ControllerInterface::Backend>(i))));
  }

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pauseOnStart, "Main", "StartPaused", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.startFullscreen, "Main", "StartFullscreen",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.renderToMain, "Main", "RenderToMainWindow", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.saveStateOnExit, "Main", "SaveStateOnExit", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.confirmPowerOff, "Main", "ConfirmPowerOff", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.loadDevicesFromSaveStates, "Main",
                                               "LoadDevicesFromSaveStates", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.applyGameSettings, "Main", "ApplyGameSettings",
                                               true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.autoLoadCheats, "Main", "AutoLoadCheats", false);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableSpeedLimiter, "Main", "SpeedLimiterEnabled",
                                               true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.increaseTimerResolution, "Main",
                                               "IncreaseTimerResolution", true);
  SettingWidgetBinder::BindWidgetToNormalizedSetting(m_host_interface, m_ui.emulationSpeed, "Main", "EmulationSpeed",
                                                     100.0f, 1.0f);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    m_host_interface, m_ui.controllerBackend, "Main", "ControllerBackend", &ControllerInterface::ParseBackendName,
    &ControllerInterface::GetBackendName, ControllerInterface::GetDefaultBackend());

  connect(m_ui.enableSpeedLimiter, &QCheckBox::stateChanged, this,
          &GeneralSettingsWidget::onEnableSpeedLimiterStateChanged);
  connect(m_ui.emulationSpeed, &QSlider::valueChanged, this, &GeneralSettingsWidget::onEmulationSpeedValueChanged);

  onEnableSpeedLimiterStateChanged();
  onEmulationSpeedValueChanged(m_ui.emulationSpeed->value());

  dialog->registerWidgetHelp(
    m_ui.confirmPowerOff, tr("Confirm Power Off"), tr("Checked"),
    tr("Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
       "when the hotkey is pressed."));
  dialog->registerWidgetHelp(m_ui.saveStateOnExit, tr("Save State On Exit"), tr("Checked"),
                             tr("Automatically saves the emulator state when powering down or exiting. You can then "
                                "resume directly from where you left off next time."));
  dialog->registerWidgetHelp(m_ui.startFullscreen, tr("Start Fullscreen"), tr("Unchecked"),
                             tr("Automatically switches to fullscreen mode when a game is started."));
  dialog->registerWidgetHelp(
    m_ui.renderToMain, tr("Render To Main Window"), tr("Checked"),
    tr("Renders the display of the simulated console to the main window of the application, over "
       "the game list. If unchecked, the display will render in a separate window."));
  dialog->registerWidgetHelp(m_ui.pauseOnStart, tr("Pause On Start"), tr("Unchecked"),
                             tr("Pauses the emulator when a game is started."));
  dialog->registerWidgetHelp(
    m_ui.loadDevicesFromSaveStates, tr("Load Devices From Save States"), tr("Unchecked"),
    tr("When enabled, memory cards and controllers will be overwritten when save states are loaded. This can "
       "result in lost saves, and controller type mismatches. For deterministic save states, enable this option, "
       "otherwise leave disabled."));
  dialog->registerWidgetHelp(
    m_ui.applyGameSettings, tr("Apply Per-Game Settings"), tr("Checked"),
    tr("When enabled, per-game settings will be applied, and incompatible enhancements will be disabled. You should "
       "leave this option enabled except when testing enhancements with incompatible games."));
  dialog->registerWidgetHelp(
    m_ui.enableSpeedLimiter, tr("Enable Speed Limiter"), tr("Checked"),
    tr("Throttles the emulation speed to the chosen speed above. If unchecked, the emulator will "
       "run as fast as possible, which may not be playable."));
  dialog->registerWidgetHelp(
    m_ui.increaseTimerResolution, tr("Increase Timer Resolution"), tr("Checked"),
    tr("Increases the system timer resolution when emulation is started to provide more accurate "
       "frame pacing. May increase battery usage on laptops."));
  dialog->registerWidgetHelp(
    m_ui.emulationSpeed, tr("Emulation Speed"), "100%",
    tr("Sets the target emulation speed. It is not guaranteed that this speed will be reached, "
       "and if not, the emulator will run as fast as it can manage."));
  dialog->registerWidgetHelp(m_ui.controllerBackend, tr("Controller Backend"),
                             qApp->translate("ControllerInterface", ControllerInterface::GetBackendName(
                                                                      ControllerInterface::GetDefaultBackend())),
                             tr("Determines the backend which is used for controller input. Windows users may prefer "
                                "to use XInput over SDL2 for compatibility."));

  // Since this one is compile-time selected, we don't put it in the .ui file.
  int current_col = 0;
  int current_row = m_ui.formLayout_4->rowCount() - current_col;
#ifdef WITH_DISCORD_PRESENCE
  {
    QCheckBox* enableDiscordPresence = new QCheckBox(tr("Enable Discord Presence"), m_ui.groupBox_4);
    SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, enableDiscordPresence, "Main",
                                                 "EnableDiscordPresence");
    m_ui.formLayout_4->addWidget(enableDiscordPresence, current_row, current_col);
    dialog->registerWidgetHelp(enableDiscordPresence, tr("Enable Discord Presence"), tr("Unchecked"),
                               tr("Shows the game you are currently playing as part of your profile in Discord."));
    current_col++;
    current_row += (current_col / 2);
    current_col %= 2;
  }
#endif
  if (AutoUpdaterDialog::isSupported())
  {
    QCheckBox* enableDiscordPresence = new QCheckBox(tr("Enable Automatic Update Check"), m_ui.groupBox_4);
    SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, enableDiscordPresence, "AutoUpdater",
                                                 "CheckAtStartup", true);
    m_ui.formLayout_4->addWidget(enableDiscordPresence, current_row, current_col);
    dialog->registerWidgetHelp(enableDiscordPresence, tr("Enable Automatic Update Check"), tr("Checked"),
                               tr("Automatically checks for updates to the program on startup. Updates can be deferred "
                                  "until later or skipped entirely."));
    current_col++;
    current_row += (current_col / 2);
    current_col %= 2;
  }
}

GeneralSettingsWidget::~GeneralSettingsWidget() = default;

void GeneralSettingsWidget::onEnableSpeedLimiterStateChanged()
{
  m_ui.emulationSpeed->setDisabled(!m_ui.enableSpeedLimiter->isChecked());
}

void GeneralSettingsWidget::onEmulationSpeedValueChanged(int value)
{
  m_ui.emulationSpeedLabel->setText(tr("%1%").arg(value));
}
