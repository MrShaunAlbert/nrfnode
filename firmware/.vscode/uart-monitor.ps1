$port = [System.IO.Ports.SerialPort]::new(
	'COM3',
	115200,
	[System.IO.Ports.Parity]::None,
	8,
	[System.IO.Ports.StopBits]::One
)
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 1000
$port.NewLine = "`n"

try {
	$port.Open()
	Write-Host 'Listening on COM3 at 115200 8N1. Stop the task with Ctrl+C.'

	while ($true) {
		try {
			$line = $port.ReadLine().TrimEnd([char]13)
			Write-Host $line
		}
		catch [System.TimeoutException] {
		}
	}
}
finally {
	if ($port.IsOpen) {
		$port.Close()
	}
	$port.Dispose()
}
