# ---------------------------------------------------------------------------
#  NanoAlphaPlayer - アプリアイコンの生成スクリプト
#
#    powershell -ExecutionPolicy Bypass -File resources\make_icon.ps1
#
#  生成物: resources\nanoalphaplayer.ico  (PNG 尺め込み / 16-256px)
#  ロゴ:   角丸スクエア + 再生三角形。コンパイル不要のためスクリプトで生成する。
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$outDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$outFile = Join-Path $outDir 'nanoalphaplayer.ico'
$sizes   = @(16, 24, 32, 48, 64, 128, 256)

function New-RoundRectPath {
    param([single]$x, [single]$y, [single]$w, [single]$h, [single]$r)
    $p = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $d = $r * 2
    $p.AddArc($x,             $y,             $d, $d, 180, 90)
    $p.AddArc($x + $w - $d,   $y,             $d, $d, 270, 90)
    $p.AddArc($x + $w - $d,   $y + $h - $d,   $d, $d, 0,   90)
    $p.AddArc($x,             $y + $h - $d,   $d, $d, 90,  90)
    $p.CloseFigure()
    return $p
}

function New-Frame {
    param([int]$size)

    $fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    $bmp = [System.Drawing.Bitmap]::new($size, $size, $fmt)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # 全座標を 256 基準で設計し、size/256 で拡縮する
    $s   = $size / 256.0
    $rad = [single](58 * $s)
    $bgRect = [System.Drawing.RectangleF]::new([single]0, [single]0, [single]$size, [single]$size)
    $path = New-RoundRectPath -x ([single](8 * $s)) -y ([single](8 * $s)) `
                              -w ([single](240 * $s)) -h ([single](240 * $s)) -r $rad

    # 背景 (上→下 のグラデーション)
    $c1 = [System.Drawing.Color]::FromArgb(255, 0x27, 0x35, 0x4B)
    $c2 = [System.Drawing.Color]::FromArgb(255, 0x0C, 0x14, 0x1E)
    $bg = [System.Drawing.Drawing2D.LinearGradientBrush]::new($bgRect, $c1, $c2, [single]90.0)
    $g.FillPath($bg, $path)

    # ボーダー
    $penColor = [System.Drawing.Color]::FromArgb(255, 0x44, 0x5A, 0x76)
    $pen = [System.Drawing.Pen]::new($penColor, [single](3.0 * $s))
    $g.DrawPath($pen, $path)

    # 再生三角形 (斜めグラデーション)
    $tri = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $tri.AddLine([single](96 * $s), [single](74  * $s), [single](96 * $s),  [single](182 * $s))
    $tri.AddLine([single](96 * $s), [single](182 * $s), [single](186 * $s), [single](128 * $s))
    $tri.CloseFigure()
    $f1 = [System.Drawing.Color]::FromArgb(255, 0x5A, 0xEE, 0xDE)
    $f2 = [System.Drawing.Color]::FromArgb(255, 0x22, 0x8B, 0xD6)
    $fRect = [System.Drawing.RectangleF]::new([single](90 * $s), [single](70 * $s),
                                              [single](100 * $s), [single](115 * $s))
    $fg = [System.Drawing.Drawing2D.LinearGradientBrush]::new($fRect, $f1, $f2, [single]35.0)
    $g.FillPath($fg, $tri)

    $pen.Dispose(); $bg.Dispose(); $fg.Dispose(); $path.Dispose(); $tri.Dispose()
    $g.Dispose()
    return $bmp
}

# --- ICO コンテナへ PNG を詰めて書き出す -----------------------------------
$ms = [System.IO.MemoryStream]::new()
$bw = [System.IO.BinaryWriter]::new($ms)
$bw.Write([uint16]0)             # reserved
$bw.Write([uint16]1)             # type = icon
$bw.Write([uint16]$sizes.Count)  # count

$blobs = @()
foreach ($sz in $sizes) {
    $bmp = New-Frame -size $sz
    $ps  = [System.IO.MemoryStream]::new()
    $bmp.Save($ps, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $blobs += , @($ps.ToArray(), $sz)
    $ps.Dispose()
}

$offset = 6 + (16 * $sizes.Count)
foreach ($entry in $blobs) {
    $data = $entry[0]
    $sz   = $entry[1]
    $dim  = if ($sz -ge 256) { [byte]0 } else { [byte]$sz }
    $bw.Write($dim)              # width  (0 = 256)
    $bw.Write($dim)              # height (0 = 256)
    $bw.Write([byte]0)           # palette colours
    $bw.Write([byte]0)           # reserved
    $bw.Write([uint16]1)         # colour planes
    $bw.Write([uint16]32)        # bits per pixel
    $bw.Write([uint32]$data.Length)
    $bw.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($entry in $blobs) { $bw.Write($entry[0]) }

[System.IO.File]::WriteAllBytes($outFile, $ms.ToArray())
$bw.Dispose()
$ms.Dispose()

"icon -> $outFile ($((Get-Item $outFile).Length) bytes, $($sizes.Count) frames)"
