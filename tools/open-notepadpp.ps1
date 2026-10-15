param([string[]]$Files)
$paths = @('E:\dev(dave)\projects\Maxos_vehicle_revnres\PLAN.md','E:\dev(dave)\projects\Maxos_vehicle_revnres\QUESTIONNAIRE.md')
if ($Files) { $paths = $Files }
Start-Process 'C:\Program Files\Notepad++\notepad++.exe' -ArgumentList ($paths | ForEach-Object { '"{0}"' -f $_ })
