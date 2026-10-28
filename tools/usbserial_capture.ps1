# usbserial_capture.ps1 -Out FILE [-Seconds N]: the iOS kernel's USB serial console (QcomXnuUsbDebug, CDC ACM,
# VID 1209 PID 316D; bsd/dev/pandora_usb.c with pandora_usb=2 qcom-usb-debug=1) into FILE, raw bytes as they come.
# Waits for the COM port to appear (Windows binds usbser to it), opens it with DTR set (the device sends only to a
# host that has raised DTR), reads with a short timeout until the port goes away, then waits for it again, until
# N seconds have passed. Lines about the port itself go to FILE.events next to it.
param(
	[Parameter(Mandatory = $true)][string]$Out,
	[int]$Seconds = 600
)

$deadline = (Get-Date).AddSeconds($Seconds)
$events = "$Out.events"
$stream = [System.IO.File]::Open($Out, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write,
	[System.IO.FileShare]::Read)
function Note([string]$text) {
	$line = "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $text
	Add-Content -Path $events -Value $line -Encoding utf8
}

function Find-Port {
	$dev = Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'USB\\VID_1209&PID_316D%'" -ErrorAction SilentlyContinue |
		Where-Object { $_.Name -match '\((COM\d+)\)' } | Select-Object -First 1
	if ($dev -and $dev.Name -match '\((COM\d+)\)') { return $Matches[1] }
	return $null
}

Note "waiting for VID_1209 PID_316D until $($deadline.ToString('HH:mm:ss'))"
$buffer = New-Object byte[] 65536
try {
	while ((Get-Date) -lt $deadline) {
		$name = Find-Port
		if (-not $name) { Start-Sleep -Milliseconds 500; continue }
		$port = New-Object System.IO.Ports.SerialPort $name, 115200
		$port.ReadTimeout = 150
		$port.DtrEnable = $true
		$port.RtsEnable = $true
		$opened = $false
		for ($try = 0; $try -lt 6 -and -not $opened; $try++) {
			try { Start-Sleep -Milliseconds 500; $port.Open(); $opened = $true } catch { Note "open $name failed: $($_.Exception.Message)" }
		}
		if (-not $opened) { continue }
		Note "opened $name"
		$total = 0
		try {
			while ((Get-Date) -lt $deadline) {
				try {
					$n = $port.Read($buffer, 0, $buffer.Length)
					if ($n -gt 0) { $stream.Write($buffer, 0, $n); $stream.Flush(); $total += $n }
				} catch [System.TimeoutException] { }
			}
		} catch {
			Note "$name gone after $total bytes: $($_.Exception.Message)"
		} finally {
			try { $port.Close() } catch { }
		}
	}
} finally {
	$stream.Close()
	Note "done"
}
