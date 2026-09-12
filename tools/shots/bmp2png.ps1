param([string]$src, [string]$dst, [int]$scaleDiv = 4)
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile($src)
$w = [int]($img.Width / $scaleDiv); $h = [int]($img.Height / $scaleDiv)
$out = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($out)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.DrawImage($img, 0, 0, $w, $h)
$g.Dispose()
$out.Save($dst, [System.Drawing.Imaging.ImageFormat]::Png)
"$src $($img.Width)x$($img.Height) -> $dst ${w}x${h}"
$out.Dispose(); $img.Dispose()
