; Textos del programa de instalación de Taxi Cam. El inglés es el original; cada idioma de [Languages]
; de taxi-cam.iss tiene un archivo con las mismas claves. El programa de instalación rellena %1.
; Se usa «usted», como en los mensajes del asistente de Inno Setup (Spanish.isl).

[CustomMessages]
StartupOptionInvalid=La opción /STARTUP debe ser automatic o manual.
InstallFailed=Error en la instalación. Cierre MSFS y Taxi Cam, y compruebe las rutas seleccionadas.
SimulatorPageDescription=Seleccione la carpeta Content del simulador
SimulatorPageSubCaption=Elija la carpeta que contiene FlightSimulator2024.exe.
SimulatorFolderLabel=Carpeta Content del simulador:
StartupPageCaption=Preferencia de inicio
StartupPageDescription=Elija cómo iniciar Taxi Cam
StartupPageSubCaption=Puede volver a intentar el inicio automático ejecutando de nuevo el programa de instalación. Si no puede configurarlo de forma segura, Taxi Cam se instalará igualmente para iniciarlo de forma manual.
StartupAutomatic=Configurar el inicio automático con MSFS
StartupManual=Iniciar manualmente; no cambiar las entradas de inicio existentes
XmlPageCaption=Inicio automático
XmlPageDescription=Seleccione el archivo exe.xml del simulador
XmlPageSubCaption=El programa de instalación conserva las demás entradas de inicio y agrega Taxi Cam. Se puede crear un nuevo exe.xml en la ruta seleccionada.
XmlFileLabel=Configuración de inicio del simulador:
XmlFileFilter=Archivos XML|*.xml|Todos los archivos|*.*
SettingsPageCaption=Configuración guardada
SettingsPageDescription=Conserve su configuración o empiece con los valores predeterminados
SettingsPageSubCaption=Conserve sus perfiles de cámara, la calibración, las guías de referencia y los atajos de teclado. La velocidad de fotogramas de las cámaras se establece en 10 (mínimo 5, intervalo 5–60). Desactive esta casilla para restablecer la configuración guardada a los valores predeterminados.
KeepExistingSettings=&Conservar la configuración existente (recomendado)
PreviousSettingsUnreadable=No se puede leer la configuración de la instalación anterior. Revise installation.json.
SimulatorFolderInvalid=Seleccione la carpeta Content que contiene FlightSimulator2024.exe.
XmlPathInvalid=Elija una ruta completa que termine en exe.xml.
ConfigureFailed=Taxi Cam se instaló, pero el programa de instalación no pudo terminar de configurarlo: %1 Vuelva a ejecutar el programa de instalación o abra Taxi Cam desde el menú Inicio cuando use MSFS.
ManualStartupNotice=Abra Taxi Cam desde el menú Inicio antes de usar MSFS. Las entradas de inicio existentes del simulador no se han modificado. Puede volver a ejecutar el programa de instalación para configurar el inicio automático.
InstalledSuccessfully=Taxi Cam se instaló correctamente.
KeepSettingsQuestion=¿Desea conservar la configuración de Taxi Cam?
KeepSettingsExplanation=Conserve sus perfiles de cámara, la calibración, las guías de referencia y los atajos de teclado para una instalación futura. Los registros se conservan en cualquier caso.
UninstallKeepSettings=&Conservar la configuración (recomendado)
UninstallKeepSettingsNote=Desinstalar la aplicación y conservar la configuración guardada.
UninstallRemoveSettings=&Quitar la configuración guardada
UninstallRemoveSettingsNote=Desinstalar la aplicación y quitar la configuración guardada.
UninstallCancel=Ca&ncelar
