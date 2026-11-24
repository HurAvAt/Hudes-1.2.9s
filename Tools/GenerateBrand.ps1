$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path (Split-Path -Parent $PSScriptRoot) 'Assets'
$mark = ([xml](Get-Content -LiteralPath "$assets/hudes-mark.svg" -Raw)).DocumentElement
$type = ([xml](Get-Content -LiteralPath "$assets/hudes-type.svg" -Raw)).DocumentElement

function Draw-Paths($g, $svg, [string]$ink) {
    foreach ($node in $svg.SelectNodes("*[local-name()='path']")) {
        $commands = [regex]::Matches($node.GetAttribute('d'), '[MLCZ][^MLCZ]*')
        $path = [Drawing.Drawing2D.GraphicsPath]::new()
        foreach ($command in $commands) {
            $coords = @([regex]::Matches($command.Value.Substring(1), '-?\d+(?:\.\d+)?') | ForEach-Object { [single]::Parse($_.Value,[Globalization.CultureInfo]::InvariantCulture) })
            switch ($command.Value[0]) {
                'M' { $path.StartFigure(); $x,$y=$coords }
                'L' { $path.AddLine($x,$y,$coords[0],$coords[1]); $x,$y=$coords }
                'C' { $path.AddBezier($x,$y,$coords[0],$coords[1],$coords[2],$coords[3],$coords[4],$coords[5]); $x,$y=$coords[4..5] }
                'Z' { $path.CloseFigure() }
            }
        }
        if ($node.GetAttribute('fill') -ne 'none') {
            $hex = if ($ink) { $ink } else { $node.GetAttribute('fill') }
            $brush = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($hex))
            $g.FillPath($brush,$path); $brush.Dispose()
        }
        if ($node.HasAttribute('stroke')) {
            $pen = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml($node.GetAttribute('stroke')),[single]$node.GetAttribute('stroke-width'))
            $pen.StartCap=$pen.EndCap=[Drawing.Drawing2D.LineCap]::Round
            $pen.LineJoin=[Drawing.Drawing2D.LineJoin]::Round
            $g.DrawPath($pen,$path); $pen.Dispose()
        }
        $path.Dispose()
    }
}

function Render([int]$width,[int]$height,[bool]$withType,[string]$ink) {
    $bitmap = [Drawing.Bitmap]::new($width*4,$height*4)
    $g = [Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = 'AntiAlias'
    if ($withType) {
        $g.ScaleTransform([single]($width*4/304),[single]($height*4/72))
        $state=$g.Save(); $g.ScaleTransform(0.75,0.75); Draw-Paths $g $mark ''; $g.Restore($state)
        $g.TranslateTransform(78,19); Draw-Paths $g $type $ink
    } else {
        $g.ScaleTransform([single]($width*4/96),[single]($height*4/96)); Draw-Paths $g $mark ''
    }
    $g.Dispose()
    $final=[Drawing.Bitmap]::new($width,$height)
    $g=[Drawing.Graphics]::FromImage($final)
    $g.InterpolationMode='HighQualityBicubic'; $g.PixelOffsetMode='HighQuality'
    $g.DrawImage($bitmap,[Drawing.Rectangle]::new(0,0,$width,$height),0,0,$bitmap.Width,$bitmap.Height,[Drawing.GraphicsUnit]::Pixel)
    $g.Dispose(); $bitmap.Dispose()
    return $final
}

foreach ($variant in @(@('hudes-logo','#183964'),@('hudes-logo-dark','#D8EAFF'))) {
    $name,$ink = $variant
    $bitmap = Render 912 216 $true $ink
    $bitmap.Save("$assets/$name.png",[Drawing.Imaging.ImageFormat]::Png); $bitmap.Dispose()
    $wordmark=$type.InnerXml.Replace('#183964',$ink)
    $svg = "<svg xmlns=""http://www.w3.org/2000/svg"" viewBox=""0 0 304 72"" role=""img"" aria-label=""Hudes""><g transform=""scale(.75)"">$($mark.InnerXml)</g><g transform=""translate(78 19)"">$wordmark</g></svg>"
    [IO.File]::WriteAllText("$assets/$name.svg",$svg)
}
$frames = foreach ($size in 16,20,24,32,40,48,64,128,256) {
    $bitmap=Render $size $size $false ''
    $png=[IO.MemoryStream]::new(); $bitmap.Save($png,[Drawing.Imaging.ImageFormat]::Png)
    [pscustomobject]@{Size=$size; Bytes=$png.ToArray()}; $png.Dispose(); $bitmap.Dispose()
}
$writer=[IO.BinaryWriter]::new([IO.File]::Create("$assets/hudes.ico"))
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
    $offset=6+16*$frames.Count
    foreach ($frame in $frames) {
        $side=[byte]($frame.Size % 256)
        $writer.Write($side); $writer.Write($side); $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.Bytes.Length); $writer.Write([uint32]$offset); $offset+=$frame.Bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
} finally { $writer.Dispose() }
