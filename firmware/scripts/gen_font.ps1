# Genererar app/src/font_unscii_8x16.inc ur Unscii (public domain, https://viznut.fi/unscii/).
# Varje 8x8-tecken dubbleras i höjd till 8x16 och packas i Zephyr CFB-format
# (CFB_FONT_MONO_VPACKED, LSB överst): två bytes per kolumn, tecken 32-255 (Latin-1).
# Anvandning: powershell -ExecutionPolicy Bypass -File scripts\gen_font.ps1 [-Preview "ABC"]
param(
	[string]$Ttf = "$PSScriptRoot\..\modules\lib\gui\lvgl\scripts\built_in_font\unscii-8.ttf",
	[string]$Out = "$PSScriptRoot\..\app\src\font_unscii_8x16.inc",
	[string]$Preview = ""
)

Add-Type -AssemblyName System.Drawing

$collection = New-Object System.Drawing.Text.PrivateFontCollection
$collection.AddFontFile((Resolve-Path $Ttf).Path)
$font = New-Object System.Drawing.Font($collection.Families[0], 8, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$format = [System.Drawing.StringFormat]::GenericTypographic
$format.FormatFlags = $format.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces

function Get-Glyph([int]$code) {
	$bitmap = New-Object System.Drawing.Bitmap 8, 8
	$g = [System.Drawing.Graphics]::FromImage($bitmap)
	$g.Clear([System.Drawing.Color]::White)
	$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixel
	$g.DrawString([string][char]$code, $font, [System.Drawing.Brushes]::Black, 0, 0, $format)
	$rows = @()
	for ($y = 0; $y -lt 8; $y++) {
		$row = 0
		for ($x = 0; $x -lt 8; $x++) {
			if ($bitmap.GetPixel($x, $y).R -lt 128) { $row = $row -bor (1 -shl (7 - $x)) }
		}
		$rows += $row
	}
	$g.Dispose()
	$bitmap.Dispose()
	return ,$rows
}

if ($Preview) {
	foreach ($ch in $Preview.ToCharArray()) {
		"--- $ch"
		$rows = Get-Glyph ([int]$ch)
		foreach ($r in $rows) {
			(0..7 | ForEach-Object { if ($r -band (1 -shl (7 - $_))) { '#' } else { '.' } }) -join ''
		}
	}
	return
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// Genererad av scripts/gen_font.ps1 ur Unscii 8 (public domain). Redigera inte for hand.")
[void]$sb.AppendLine("static const uint8_t font_unscii_8x16[224][16] = {")
for ($code = 32; $code -le 255; $code++) {
	$rows = Get-Glyph $code
	$bytes = @()
	for ($x = 0; $x -lt 8; $x++) {
		$bits = 0
		for ($y = 0; $y -lt 8; $y++) {
			if ($rows[$y] -band (1 -shl (7 - $x))) { $bits = $bits -bor (3 -shl (2 * $y)) }
		}
		$bytes += ('0x{0:x2}' -f ($bits -band 0xFF))
		$bytes += ('0x{0:x2}' -f (($bits -shr 8) -band 0xFF))
	}
	[void]$sb.AppendLine("`t{ " + ($bytes -join ', ') + " }, /* $code */")
}
[void]$sb.AppendLine("};")
[System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($Out), $sb.ToString())
"Wrote $Out"
