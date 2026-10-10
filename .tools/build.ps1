#Requires -Version 5.1

# --- DLL build and deployment conventions ---
# Parameterized driver shared by the ykkz000 mod repos. Only the targets listed here are built and
# deployed; set $LoaderTarget/$LoaderBuildDll to $null in a repo that ships no loader.
#
# Loader repo: builds the GameCore proxy loader and deploys it as <DllPrefix>_FinalRelease.dll.
# DllPrefix must match the DllPrefix used in Data/YKKZ000_GameCores.sql.
$LoaderTarget     = 'YKKZ000Loader'
$LoaderBuildDll   = 'GameCore_YKKZ000_Loader_XP2.dll'
# No plugins are produced by the loader repo.
$PluginTargets    = @()
$PluginDlls       = @()
$DeployBinRel     = 'Binaries/Win64'
$DeployPluginRel  = 'Binaries/Win64/ykkz000_civ6_plugin'
$DefaultDllPrefix = 'GameCore_YKKZ000_Loader_XP2'
$DefaultDllConfig = 'RelWithDebInfo'

function Get-UsageText {
  return @"
Usage: build.ps1 <project.civ6proj> [options]

Builds a Civilization VI mod project without ModBuddy or MSBuild. The project
file is parsed, content files are copied, the Firaxis asset cooker is run, and
the .modinfo file is generated.

Arguments:
  <project.civ6proj>        Path to the project file to build (required).

Options:
  --base-assets <path>      Civilization VI SDK Assets root.
                            Default: C:\Program Files (x86)\Steam\steamapps\
                            common\Sid Meier's Civilization VI SDK Assets
  --base-sdk <path>         Civilization VI SDK root.
                            Default: C:\Program Files (x86)\Steam\steamapps\
                            common\Sid Meier's Civilization VI SDK
  --clean                   Delete the output directory and the DLL build
                            directory before building.
  --cmake <path>            Path to cmake.exe.
                            Default: cmake found on PATH.
  --cmake-arg <arg>         Extra argument passed to the CMake configure step
                            (for example -D<option>=<value>); may be repeated.
                            Use the --cmake-arg=<arg> form for values that
                            start with '-'.
                            Default: (none)
  --dll-build-dir <path>    CMake build directory; removed by --clean or
                            --force-rebuild=dll.
                            Default: <dll-dir>\build
  -d, --debug               Build the DLLs in the Debug configuration, which
                            defines the standard _DEBUG macro and enables debug
                            logging. Equivalent to --dll-config Debug.
  --no-debug, --release     Build the DLLs in the default non-debug
                            configuration (RelWithDebInfo). This is the default.
  --dll-config <cfg>        CMake build configuration. Cannot be combined with
                            --debug/--no-debug.
                            Default: RelWithDebInfo
  --dll-dir <path>          CMake source directory for the DLLs.
                            Default: <projectDir>\natives
  --dll-generator <gen>     CMake generator for the DLL build.
                            Default: the CMake default (Visual Studio).
  --vcpkg-root <path>       vcpkg installation root used to resolve the
                            manifest dependencies (minhook, log4cxx).
                            Default: the VCPKG_ROOT environment variable.
  --vcpkg-triplet <t>       vcpkg target triplet for the DLL dependencies.
                            Default: x64-windows-static
  --dll-prefix <name>       Deployed loader DLL prefix; must match the
                            DllPrefix in Data/YKKZ000_GameCores.sql.
                            Default: GameCore_YKKZ000_Loader_XP2
  --force-rebuild <targets> Force a clean rebuild of the given targets.
                            <targets> is a comma-separated list of all, dll,
                            art; the option may be repeated. dll deletes the
                            CMake build directory; art deletes the cooked art
                            and the .dep before cooking.
                            Default: (none)
  -h, --help                Show this help and exit.
  -o, --output <path>       Build output directory.
                            Default: <My Documents>\My Games\Sid Meier's
                            Civilization VI\Mods\<projectName>
  --skip <targets>          Skip the given targets. <targets> is a
                            comma-separated list of all, dll, art; the option
                            may be repeated. dll reuses the DLLs already
                            deployed; art reuses the art already cooked in the
                            output directory.
                            Default: (none)

Examples:
  .\build.ps1 "ykkz000's Soviet Union (Rework).civ6proj"
  .\build.ps1 mod.civ6proj --clean -o "C:\temp\mod-build"
  .\build.ps1 "ykkz000's Soviet Union (Rework).civ6proj" --force-rebuild=dll
  .\build.ps1 "ykkz000's Soviet Union (Rework).civ6proj" --skip=art,dll
  .\build.ps1 "ykkz000's Soviet Union (Rework).civ6proj" --force-rebuild=dll --cmake-arg=-DDISABLE_STRENGTH_PER_SUZERAIN=ON
  .\build.ps1 "ykkz000's Soviet Union (Rework).civ6proj" --debug
  .\build.ps1 "ykkz000's Soviet Union (Rework).civ6proj" --vcpkg-root C:\vcpkg
"@
}

function Add-RevisionTarget {
  param(
    [System.Collections.Generic.HashSet[string]]$Set,
    [string]$Value,
    [string]$OptionName
  )
  foreach ($part in ($Value -split ',')) {
    $token = $part.Trim().ToLowerInvariant()
    if ([string]::IsNullOrEmpty($token)) {
      throw "Option '$OptionName' has an empty value; expected a comma-separated list of: all, dll, art."
    }
    switch ($token) {
      'all' { [void]$Set.Add('dll'); [void]$Set.Add('art') }
      'dll' { [void]$Set.Add('dll') }
      'art' { [void]$Set.Add('art') }
      default { throw "Invalid value '$token' for '$OptionName'; expected one of: all, dll, art." }
    }
  }
}

function Parse-Args {
  param([string[]]$ArgList)

  if ($null -eq $ArgList) { $ArgList = @() }

  $opts = [ordered]@{
    Input        = $null
    Output       = $null
    Skip         = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    ForceRebuild = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    Clean        = $false
    BaseAssets   = "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization VI SDK Assets"
    BaseSdk      = "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization VI SDK"
    CMake        = $null
    CMakeArgs    = [System.Collections.Generic.List[string]]::new()
    DllDir       = $null
    DllBuildDir  = $null
    DllPrefix    = $DefaultDllPrefix
    DllConfig    = $DefaultDllConfig
    DllConfigExplicit = $false
    Debug        = $false
    NoDebug      = $false
    DllGenerator = $null
    VcpkgRoot    = $env:VCPKG_ROOT
    VcpkgTriplet = 'x64-windows-static'
    Help         = $false
  }

  $i = 0
  while ($i -lt $ArgList.Count) {
    $a = $ArgList[$i]
    if ($a -eq '-h' -or $a -eq '--help') {
      $opts.Help = $true
      $i++
    }
    elseif ($a -eq '-o' -or $a -eq '--output') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.Output = $v
      $i += 2
    }
    elseif ($a -like '--output=*') {
      $v = $a.Substring('--output='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--output' requires a value." }
      $opts.Output = $v
      $i++
    }
    elseif ($a -eq '--base-assets') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.BaseAssets = $v
      $i += 2
    }
    elseif ($a -like '--base-assets=*') {
      $v = $a.Substring('--base-assets='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--base-assets' requires a value." }
      $opts.BaseAssets = $v
      $i++
    }
    elseif ($a -eq '--base-sdk') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.BaseSdk = $v
      $i += 2
    }
    elseif ($a -like '--base-sdk=*') {
      $v = $a.Substring('--base-sdk='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--base-sdk' requires a value." }
      $opts.BaseSdk = $v
      $i++
    }
    elseif ($a -eq '--skip') {
      if ($i + 1 -lt $ArgList.Count -and -not $ArgList[$i + 1].StartsWith('-')) {
        Add-RevisionTarget -Set $opts.Skip -Value $ArgList[$i + 1] -OptionName '--skip'
        $i += 2
      }
      else {
        Add-RevisionTarget -Set $opts.Skip -Value 'all' -OptionName '--skip'
        $i++
      }
    }
    elseif ($a -like '--skip=*') {
      Add-RevisionTarget -Set $opts.Skip -Value $a.Substring('--skip='.Length) -OptionName '--skip'
      $i++
    }
    elseif ($a -eq '--force-rebuild') {
      if ($i + 1 -lt $ArgList.Count -and -not $ArgList[$i + 1].StartsWith('-')) {
        Add-RevisionTarget -Set $opts.ForceRebuild -Value $ArgList[$i + 1] -OptionName '--force-rebuild'
        $i += 2
      }
      else {
        Add-RevisionTarget -Set $opts.ForceRebuild -Value 'all' -OptionName '--force-rebuild'
        $i++
      }
    }
    elseif ($a -like '--force-rebuild=*') {
      Add-RevisionTarget -Set $opts.ForceRebuild -Value $a.Substring('--force-rebuild='.Length) -OptionName '--force-rebuild'
      $i++
    }
    elseif ($a -eq '--clean') {
      $opts.Clean = $true
      $i++
    }
    elseif ($a -eq '--cmake') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.CMake = $v
      $i += 2
    }
    elseif ($a -like '--cmake=*') {
      $v = $a.Substring('--cmake='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--cmake' requires a value." }
      $opts.CMake = $v
      $i++
    }
    elseif ($a -eq '--cmake-arg') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.CMakeArgs.Add($v)
      $i += 2
    }
    elseif ($a -like '--cmake-arg=*') {
      $v = $a.Substring('--cmake-arg='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--cmake-arg' requires a value." }
      $opts.CMakeArgs.Add($v)
      $i++
    }
    elseif ($a -eq '--dll-dir') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.DllDir = $v
      $i += 2
    }
    elseif ($a -like '--dll-dir=*') {
      $v = $a.Substring('--dll-dir='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--dll-dir' requires a value." }
      $opts.DllDir = $v
      $i++
    }
    elseif ($a -eq '--dll-build-dir') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.DllBuildDir = $v
      $i += 2
    }
    elseif ($a -like '--dll-build-dir=*') {
      $v = $a.Substring('--dll-build-dir='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--dll-build-dir' requires a value." }
      $opts.DllBuildDir = $v
      $i++
    }
    elseif ($a -eq '--dll-prefix') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.DllPrefix = $v
      $i += 2
    }
    elseif ($a -like '--dll-prefix=*') {
      $v = $a.Substring('--dll-prefix='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--dll-prefix' requires a value." }
      $opts.DllPrefix = $v
      $i++
    }
    elseif ($a -eq '--dll-config') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.DllConfig = $v
      $opts.DllConfigExplicit = $true
      $i += 2
    }
    elseif ($a -like '--dll-config=*') {
      $v = $a.Substring('--dll-config='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--dll-config' requires a value." }
      $opts.DllConfig = $v
      $opts.DllConfigExplicit = $true
      $i++
    }
    elseif ($a -eq '--dll-generator') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.DllGenerator = $v
      $i += 2
    }
    elseif ($a -like '--dll-generator=*') {
      $v = $a.Substring('--dll-generator='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--dll-generator' requires a value." }
      $opts.DllGenerator = $v
      $i++
    }
    elseif ($a -eq '--vcpkg-root') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.VcpkgRoot = $v
      $i += 2
    }
    elseif ($a -like '--vcpkg-root=*') {
      $v = $a.Substring('--vcpkg-root='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--vcpkg-root' requires a value." }
      $opts.VcpkgRoot = $v
      $i++
    }
    elseif ($a -eq '--vcpkg-triplet') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.VcpkgTriplet = $v
      $i += 2
    }
    elseif ($a -like '--vcpkg-triplet=*') {
      $v = $a.Substring('--vcpkg-triplet='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--vcpkg-triplet' requires a value." }
      $opts.VcpkgTriplet = $v
      $i++
    }
    elseif ($a -eq '-d' -or $a -eq '--debug') {
      $opts.Debug = $true
      $i++
    }
    elseif ($a -eq '--no-debug' -or $a -eq '--release') {
      $opts.NoDebug = $true
      $i++
    }
    elseif ($a.Length -gt 1 -and $a[0] -eq '-') {
      throw "Unknown option: $a"
    }
    else {
      if ($null -ne $opts.Input) { throw "Unexpected argument: $a" }
      $opts.Input = $a
      $i++
    }
  }

  return [pscustomobject]$opts
}

function Resolve-FullPath {
  param([string]$Path)
  if ([System.IO.Path]::IsPathRooted($Path)) {
    return [System.IO.Path]::GetFullPath($Path)
  }
  $base = (Get-Location).Path
  return [System.IO.Path]::GetFullPath((Join-Path $base $Path))
}

function Remove-TrailingSeparator {
  param([string]$Path)
  if ([string]::IsNullOrEmpty($Path)) { return $Path }
  $full = [System.IO.Path]::GetFullPath($Path)
  if ($full -eq [System.IO.Path]::GetPathRoot($full)) { return $full }
  return $full.TrimEnd('\', '/')
}

function Test-PathUnder {
  param([string]$Child, [string]$Parent)
  $c = [System.IO.Path]::GetFullPath($Child)
  $p = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
  if ([string]::IsNullOrEmpty($p)) { return $true }
  $prefix = $p + [System.IO.Path]::DirectorySeparatorChar
  return $c.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-PathEqual {
  param([string]$A, [string]$B)
  $a = Remove-TrailingSeparator $A
  $b = Remove-TrailingSeparator $B
  return ($a -eq $b)
}

function Assert-Inputs {
  param($opts)

  $skipDll  = $opts.Skip.Contains('dll')
  $skipArt  = $opts.Skip.Contains('art')
  $forceDll = $opts.ForceRebuild.Contains('dll')
  $forceArt = $opts.ForceRebuild.Contains('art')

  foreach ($name in @('dll', 'art')) {
    if ($opts.Skip.Contains($name) -and $opts.ForceRebuild.Contains($name)) {
      throw "Options '--skip=$name' and '--force-rebuild=$name' cannot be used together."
    }
  }
  if ($opts.Clean -and $skipArt) {
    throw "Options '--clean' and '--skip=art' cannot be used together."
  }

  foreach ($arg in $opts.CMakeArgs) {
    if ([string]::IsNullOrWhiteSpace($arg)) {
      throw "Option '--cmake-arg' requires a non-empty value."
    }
  }
  if ($skipDll -and $opts.CMakeArgs.Count -gt 0) {
    throw "Option '--cmake-arg' has no effect together with '--skip=dll' (the DLL build is skipped)."
  }
  if (($opts.Debug -or $opts.NoDebug) -and $opts.DllConfigExplicit) {
    throw "Options '--debug'/'--no-debug' and '--dll-config' cannot be used together."
  }
  if ($opts.Debug -and $opts.NoDebug) {
    throw "Options '--debug' and '--no-debug' cannot be used together."
  }
  if ($skipDll -and ($opts.Debug -or $opts.NoDebug)) {
    throw "Options '--debug'/'--no-debug' have no effect together with '--skip=dll'."
  }

  $inputPath = Resolve-FullPath $opts.Input
  if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
    throw "Project file not found: $inputPath"
  }
  if (-not $inputPath.EndsWith('.civ6proj', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The input file must have the '.civ6proj' extension: $inputPath"
  }

  $projectDir = Split-Path -Parent $inputPath
  $projectName = [System.IO.Path]::GetFileNameWithoutExtension($inputPath)

  $documents = [Environment]::GetFolderPath('MyDocuments')
  if ([string]::IsNullOrEmpty($documents)) {
    $documents = Join-Path $env:UserProfile 'Documents'
  }
  $defaultOut = Remove-TrailingSeparator (Join-Path $documents ("My Games\Sid Meier's Civilization VI\Mods\" + $projectName))

  if ([string]::IsNullOrEmpty($opts.Output)) {
    $out = $defaultOut
  }
  else {
    $out = Remove-TrailingSeparator (Resolve-FullPath $opts.Output)
  }

  if ([string]::IsNullOrEmpty($opts.DllDir)) {
    $dllDir = Join-Path $projectDir 'natives'
  }
  else {
    $dllDir = Remove-TrailingSeparator (Resolve-FullPath $opts.DllDir)
  }
  if ([string]::IsNullOrEmpty($opts.DllBuildDir)) {
    $dllBuildDir = Join-Path $dllDir 'build'
  }
  else {
    $dllBuildDir = Remove-TrailingSeparator (Resolve-FullPath $opts.DllBuildDir)
  }
  $dllPrefix = $opts.DllPrefix
  $dllConfig = $opts.DllConfig
  if ($opts.Debug) { $dllConfig = 'Debug' }
  $dllGenerator = $opts.DllGenerator

  if ([string]::IsNullOrWhiteSpace($dllPrefix)) {
    throw "The DLL prefix must not be empty."
  }
  if ($dllPrefix -like '*_FinalRelease*') {
    throw "The DLL prefix must not contain '_FinalRelease': $dllPrefix"
  }
  if ($dllPrefix -match '[\\/:*?"<>|]' -or $dllPrefix -eq '.' -or $dllPrefix -eq '..') {
    throw "The DLL prefix must be a bare file name without path separators: $dllPrefix"
  }

  $sqlPrefix = Read-GameCoreDllPrefix -ProjectDir $projectDir
  if (-not [string]::IsNullOrEmpty($sqlPrefix) -and $sqlPrefix -ne $dllPrefix) {
    throw "The DLL prefix '$dllPrefix' does not match the DllPrefix '$sqlPrefix' declared in Data/YKKZ000_GameCores.sql."
  }

  $cmake = $null
  $vcpkgToolchain = $null
  $vcpkgTriplet = $opts.VcpkgTriplet
  if (-not $skipDll) {
    if (-not (Test-Path -LiteralPath $dllDir -PathType Container)) {
      throw "DLL source directory not found: $dllDir"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dllDir 'CMakeLists.txt') -PathType Leaf)) {
      throw "CMakeLists.txt was not found in the DLL source directory: $dllDir"
    }
    $cmake = Resolve-CMakePath -Explicit $opts.CMake

    if ([string]::IsNullOrWhiteSpace($opts.VcpkgRoot)) {
      throw "vcpkg was not found. Set the VCPKG_ROOT environment variable (clone vcpkg and run bootstrap-vcpkg.bat) or pass --vcpkg-root <path>."
    }
    $vcpkgRoot = Remove-TrailingSeparator (Resolve-FullPath $opts.VcpkgRoot)
    if (-not (Test-Path -LiteralPath $vcpkgRoot -PathType Container)) {
      throw "vcpkg root not found: $vcpkgRoot"
    }
    $vcpkgToolchain = Join-Path (Join-Path $vcpkgRoot 'scripts\buildsystems') 'vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $vcpkgToolchain -PathType Leaf)) {
      throw "The vcpkg CMake toolchain was not found: $vcpkgToolchain (check --vcpkg-root)."
    }
    if ([string]::IsNullOrWhiteSpace($vcpkgTriplet)) {
      throw "The vcpkg triplet must not be empty."
    }
  }

  return [pscustomobject]@{
    InputPath    = $inputPath
    ProjectDir   = $projectDir
    ProjectName  = $projectName
    Out          = $out
    DefaultOut   = $defaultOut
    BaseAssets   = (Resolve-FullPath $opts.BaseAssets)
    BaseSdk      = (Resolve-FullPath $opts.BaseSdk)
    Skip         = $opts.Skip
    ForceRebuild = $opts.ForceRebuild
    SkipArt      = $skipArt
    SkipDll      = $skipDll
    ForceDll     = $forceDll
    ForceArt     = $forceArt
    Clean        = [bool]$opts.Clean
    CMake        = $cmake
    CMakeArgs    = @($opts.CMakeArgs)
    DllDir       = $dllDir
    DllBuildDir  = $dllBuildDir
    DllPrefix    = $dllPrefix
    DllConfig    = $dllConfig
    DllGenerator = $dllGenerator
    VcpkgToolchain = $vcpkgToolchain
    VcpkgTriplet = $vcpkgTriplet
  }
}

function Invoke-Clean {
  param([string]$Out, [string]$ProjectDir, [string]$DefaultOut)

  if ([string]::IsNullOrWhiteSpace($Out)) {
    throw "Refusing to clean: the output path is empty."
  }

  $fullOut = Remove-TrailingSeparator $Out
  if ([string]::IsNullOrEmpty($fullOut)) {
    throw "Refusing to clean: the output path is empty."
  }

  $root = [System.IO.Path]::GetPathRoot($fullOut)
  if (-not [string]::IsNullOrEmpty($root) -and (Test-PathEqual $fullOut $root)) {
    throw "Refusing to clean the filesystem root: $fullOut"
  }

  if (Test-PathEqual $fullOut $ProjectDir) {
    throw "Refusing to clean the project directory: $fullOut"
  }

  if (Test-PathUnder $ProjectDir $fullOut) {
    throw "Refusing to clean '$fullOut' because the project directory is inside it."
  }

  if (-not [string]::IsNullOrEmpty($DefaultOut) -and (Test-PathUnder $DefaultOut $fullOut)) {
    throw "Refusing to clean '$fullOut' because it contains the default mod output directory."
  }

  if (Test-Path -LiteralPath $fullOut) {
    Remove-Item -LiteralPath $fullOut -Recurse -Force -ErrorAction Stop
  }
}

function Read-Project {
  param([string]$Path)

  $doc = New-Object System.Xml.XmlDocument
  try {
    $doc.Load($Path)
  }
  catch {
    throw "Failed to parse the project file '$Path': $($_.Exception.Message)"
  }

  $getText = {
    param([string]$Name)
    $nodes = $doc.GetElementsByTagName($Name)
    if ($nodes.Count -eq 0) { return '' }
    return $nodes[0].InnerText
  }

  $getCdata = {
    param([string]$Name)
    $nodes = $doc.GetElementsByTagName($Name)
    if ($nodes.Count -eq 0) { return $null }
    foreach ($child in $nodes[0].ChildNodes) {
      if ($child.NodeType -eq [System.Xml.XmlNodeType]::CDATA) { return $child.Value }
    }
    return $null
  }

  $content = New-Object System.Collections.Generic.List[string]
  foreach ($node in $doc.GetElementsByTagName('Content')) {
    $inc = $node.GetAttribute('Include')
    if (-not [string]::IsNullOrEmpty($inc)) { $content.Add($inc) }
  }

  return [pscustomobject]@{
    Guid               = (& $getText 'Guid')
    ModVersion         = (& $getText 'ModVersion')
    Name               = (& $getText 'Name')
    Teaser             = (& $getText 'Teaser')
    Description        = (& $getText 'Description')
    Authors            = (& $getText 'Authors')
    CompatibleVersions = (& $getText 'CompatibleVersions')
    SpecialThanks      = (& $getText 'SpecialThanks')
    Homepage           = (& $getText 'Homepage')
    ActionCriteriaData = (& $getCdata 'ActionCriteriaData')
    FrontEndActionData = (& $getCdata 'FrontEndActionData')
    InGameActionData   = (& $getCdata 'InGameActionData')
    LocalizedTextData  = (& $getCdata 'LocalizedTextData')
    AssociationData    = (& $getCdata 'AssociationData')
    Content            = $content
  }
}

function Copy-ContentFiles {
  param($ContentList, [string]$ProjectDir, [string]$Out, [bool]$SkipArt)

  $result = New-Object System.Collections.Generic.List[string]
  foreach ($rel in $ContentList) {
    if ([System.IO.Path]::IsPathRooted($rel)) {
      throw "Content path must be relative to the project directory: $rel"
    }
    if ($SkipArt -and $rel -match '\.artdef$') { continue }

    $src = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $rel))
    if (-not (Test-PathUnder $src $ProjectDir)) {
      throw "Content path escapes the project directory: $rel"
    }
    $dst = [System.IO.Path]::GetFullPath((Join-Path $Out $rel))
    if (-not (Test-PathUnder $dst $Out)) {
      throw "Content path escapes the output directory: $rel"
    }
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
      throw "Content file listed in the project does not exist: $rel"
    }

    $dstDir = Split-Path -Parent $dst
    if (-not (Test-Path -LiteralPath $dstDir)) {
      New-Item -ItemType Directory -Path $dstDir -Force -ErrorAction Stop | Out-Null
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop
    $result.Add(($rel -replace '\\', '/'))
  }

  return [pscustomobject]@{ Count = $result.Count; Files = $result }
}

function Read-GameCoreDllPrefix {
  param([string]$ProjectDir)

  $sqlPath = Join-Path (Join-Path $ProjectDir 'Data') 'YKKZ000_GameCores.sql'
  if (-not (Test-Path -LiteralPath $sqlPath -PathType Leaf)) {
    return $null
  }
  $text = [System.IO.File]::ReadAllText($sqlPath)
  $match = [regex]::Match($text, "DllPrefix\s*=\s*'([^']*)'")
  if (-not $match.Success) {
    return $null
  }
  return $match.Groups[1].Value
}

function Resolve-CMakePath {
  param([string]$Explicit)

  if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
    $command = Get-Command $Explicit -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    $full = Resolve-FullPath $Explicit
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
      throw "cmake was not found at the given path: $full"
    }
    return $full
  }

  $command = Get-Command cmake -ErrorAction SilentlyContinue
  if ($null -eq $command) {
    throw "cmake was not found on PATH. Install CMake (https://cmake.org/download/) or pass --cmake <path>."
  }
  return $command.Source
}

function Invoke-DllBuild {
  param(
    [string]$CMake,
    [string]$DllDir,
    [string]$BuildDir,
    [string]$Config,
    [string]$Generator,
    [string]$ToolchainFile,
    [string]$Triplet,
    [string[]]$CMakeArgs = @()
  )

  if (-not (Test-Path -LiteralPath (Join-Path $DllDir 'CMakeLists.txt') -PathType Leaf)) {
    throw "CMakeLists.txt was not found in the DLL source directory: $DllDir"
  }
  if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
    New-Item -ItemType Directory -Path $BuildDir -Force -ErrorAction Stop | Out-Null
  }

  $generatorArgs = @()
  $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
  if (-not (Test-Path -LiteralPath $cacheFile -PathType Leaf)) {
    if (-not [string]::IsNullOrWhiteSpace($Generator)) {
      $generatorArgs += @('-G', $Generator)
    }
    if ([string]::IsNullOrWhiteSpace($Generator) -or $Generator -like 'Visual Studio*') {
      $generatorArgs += @('-A', 'x64')
    }
  }

  if ($null -ne $CMakeArgs -and $CMakeArgs.Count -gt 0) {
    Write-Host ("Extra CMake configure arguments: {0}" -f ($CMakeArgs -join ' '))
  }

  $isMultiConfig = [string]::IsNullOrWhiteSpace($Generator) -or $Generator -like 'Visual Studio*'
  $configureArgs = @()
  if (-not $isMultiConfig) { $configureArgs += "-DCMAKE_BUILD_TYPE=$Config" }
  if (-not [string]::IsNullOrWhiteSpace($ToolchainFile)) {
    $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
  }
  if (-not [string]::IsNullOrWhiteSpace($Triplet)) {
    $configureArgs += "-DVCPKG_TARGET_TRIPLET=$Triplet"
  }

  Write-Host ("Configuring the DLL build in {0}" -f $BuildDir)
  & $CMake -S $DllDir -B $BuildDir @generatorArgs @configureArgs @CMakeArgs
  if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed (exit $LASTEXITCODE)."
  }

  $allTargets = @(@($LoaderTarget) + $PluginTargets | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
  Write-Host ("Building DLL targets: {0}" -f ($allTargets -join ', '))
  & $CMake --build $BuildDir --config $Config --target $allTargets
  if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed (exit $LASTEXITCODE)."
  }
}

function Find-DllArtifact {
  param([string]$BuildDir, [string]$Config, [string]$FileName)

  $direct = Join-Path (Join-Path $BuildDir 'bin') $FileName
  if (Test-Path -LiteralPath $direct -PathType Leaf) { return $direct }

  $perConfig = Join-Path (Join-Path (Join-Path $BuildDir 'bin') $Config) $FileName
  if (Test-Path -LiteralPath $perConfig -PathType Leaf) { return $perConfig }

  $matches = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter $FileName -File -ErrorAction SilentlyContinue)
  if ($matches.Count -eq 1) { return $matches[0].FullName }
  if ($matches.Count -eq 0) {
    throw "The DLL artifact '$FileName' was not found under '$BuildDir'."
  }
  throw "More than one '$FileName' was found under '$BuildDir'; cannot determine the build output."
}

function Copy-DllArtifacts {
  param($Ctx)

  $deployed = New-Object System.Collections.Generic.List[string]

  if (-not [string]::IsNullOrWhiteSpace($LoaderBuildDll)) {
    $binDir = Join-Path $Ctx.Out $DeployBinRel
    New-Item -ItemType Directory -Path $binDir -Force -ErrorAction Stop | Out-Null
    $loaderSrc = Find-DllArtifact -BuildDir $Ctx.DllBuildDir -Config $Ctx.DllConfig -FileName $LoaderBuildDll
    $loaderName = $Ctx.DllPrefix + '_FinalRelease.dll'
    $loaderDst = Join-Path $binDir $loaderName
    if (-not (Test-PathUnder $loaderDst $Ctx.Out)) {
      throw "The loader deploy path escapes the output directory: $loaderDst"
    }
    Copy-Item -LiteralPath $loaderSrc -Destination $loaderDst -Force -ErrorAction Stop
    $deployed.Add($DeployBinRel + '/' + $loaderName)
  }

  $pluginDir = Join-Path $Ctx.Out $DeployPluginRel
  New-Item -ItemType Directory -Path $pluginDir -Force -ErrorAction Stop | Out-Null

  foreach ($dll in $PluginDlls) {
    $pluginSrc = Find-DllArtifact -BuildDir $Ctx.DllBuildDir -Config $Ctx.DllConfig -FileName $dll
    $pluginDst = Join-Path $pluginDir $dll
    if (-not (Test-PathUnder $pluginDst $Ctx.Out)) {
      throw "The plugin deploy path escapes the output directory: $pluginDst"
    }
    Copy-Item -LiteralPath $pluginSrc -Destination $pluginDst -Force -ErrorAction Stop
    $deployed.Add($DeployPluginRel + '/' + $dll)
  }

  return $deployed
}

function Get-DeployedDllFiles {
  param($Ctx)

  $result = New-Object System.Collections.Generic.List[string]
  $candidates = @()
  if (-not [string]::IsNullOrWhiteSpace($LoaderBuildDll)) {
    $candidates += ($DeployBinRel + '/' + ($Ctx.DllPrefix + '_FinalRelease.dll'))
  }
  foreach ($dll in $PluginDlls) {
    $candidates += ($DeployPluginRel + '/' + $dll)
  }
  foreach ($rel in $candidates) {
    $full = Join-Path $Ctx.Out ($rel -replace '/', '\')
    if (Test-Path -LiteralPath $full -PathType Leaf) {
      $result.Add($rel)
    }
  }
  return $result
}

function Remove-DllIntermediate {
  param([string]$BuildDir, [string]$ProjectDir)

  if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    throw "Refusing to clean: the DLL build directory is empty."
  }
  if (Test-PathEqual $BuildDir $ProjectDir) {
    throw "Refusing to clean the project directory: $BuildDir"
  }
  if (-not (Test-PathUnder $BuildDir $ProjectDir)) {
    throw "Refusing to clean outside the project directory: $BuildDir"
  }
  if (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeLists.txt') -PathType Leaf) {
    throw "Refusing to clean '$BuildDir': it looks like a source directory (contains CMakeLists.txt)."
  }
  if (Test-Path -LiteralPath $BuildDir) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force -ErrorAction Stop
  }
}

function Remove-CookedArt {
  param([string]$Out, [string]$ProjectDir, [string]$DefaultOut)

  if ([string]::IsNullOrWhiteSpace($Out)) {
    throw "Refusing to clean cooked art: the output path is empty."
  }
  $fullOut = Remove-TrailingSeparator $Out
  if (Test-PathEqual $fullOut $ProjectDir) {
    throw "Refusing to clean cooked art in the project directory: $fullOut"
  }
  if (-not [string]::IsNullOrEmpty($DefaultOut) -and (Test-PathUnder $DefaultOut $fullOut)) {
    throw "Refusing to clean '$fullOut' because it contains the default mod output directory."
  }

  $targets = @(
    (Join-Path $fullOut 'ArtDefs'),
    (Join-Path $fullOut 'Platforms\Windows\BLPs'),
    (Join-Path $fullOut 'Platforms\MacOS\BLPs')
  )
  foreach ($target in $targets) {
    if (-not (Test-PathUnder $target $fullOut)) {
      throw "Refusing to clean outside the output directory: $target"
    }
    if (Test-Path -LiteralPath $target) {
      Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
    }
  }
  foreach ($dep in @(Get-ChildItem -LiteralPath $fullOut -Filter *.dep -File -ErrorAction SilentlyContinue)) {
    Remove-Item -LiteralPath $dep.FullName -Force -ErrorAction Stop
  }
}

function Read-ArtXmlInfo {
  param([string]$Path)

  $text = [System.IO.File]::ReadAllText($Path)

  $id = ''
  $idMatch = [regex]::Match($text, '<id\s+text="([^"]*)"')
  if ($idMatch.Success) { $id = $idMatch.Groups[1].Value }

  $required = New-Object System.Collections.Generic.List[string]
  $reqMatch = [regex]::Match($text, '<requiredGameArtIDs>(.*?)</requiredGameArtIDs>', [System.Text.RegularExpressions.RegexOptions]::Singleline)
  if ($reqMatch.Success) {
    foreach ($m in [regex]::Matches($reqMatch.Groups[1].Value, '<id\s+text="([^"]*)"')) {
      $required.Add($m.Groups[1].Value)
    }
  }

  return [pscustomobject]@{ Id = $id; Required = $required }
}

function Resolve-PantryPaths {
  param([string]$ProjectDir, [string]$BaseAssets, $RootRequired)

  if (-not (Test-Path -LiteralPath $BaseAssets -PathType Container)) {
    throw "Base assets path not found: $BaseAssets"
  }

  try {
    $candidates = @(Get-ChildItem -LiteralPath $BaseAssets -Recurse -Filter *.Art.xml -File -ErrorAction Stop)
  }
  catch {
    throw "Failed to scan base assets at '$BaseAssets': $($_.Exception.Message)"
  }

  $map = @{}
  foreach ($file in $candidates) {
    if ($file.Directory.Name -ine 'pantry') { continue }
    try {
      $info = Read-ArtXmlInfo $file.FullName
    }
    catch {
      throw "Failed to read pantry file '$($file.FullName)': $($_.Exception.Message)"
    }
    if (-not [string]::IsNullOrEmpty($info.Id) -and -not $map.ContainsKey($info.Id)) {
      $map[$info.Id] = [pscustomobject]@{ Dir = $file.Directory.FullName; Required = $info.Required }
    }
  }

  $result = New-Object System.Collections.Generic.List[string]
  $ctx = @{ Map = $map; Visited = @{}; Result = $result; BaseAssets = $BaseAssets }

  function Visit-ArtId {
    param([string]$Id, $Ctx)
    if ($Ctx.Visited.ContainsKey($Id)) { return }
    $Ctx.Visited[$Id] = $true
    if (-not $Ctx.Map.ContainsKey($Id)) {
      throw "Required game art ID '$Id' was not found in any pantry under '$($Ctx.BaseAssets)'."
    }
    $entry = $Ctx.Map[$Id]
    foreach ($child in $entry.Required) { Visit-ArtId -Id $child -Ctx $Ctx }
    if (-not $Ctx.Result.Contains($entry.Dir)) { $Ctx.Result.Add($entry.Dir) }
  }

  foreach ($id in $RootRequired) { Visit-ArtId -Id $id -Ctx $ctx }

  return $result
}

function Get-ArtInfo {
  param([string]$ProjectDir)

  $artXmls = @(Get-ChildItem -LiteralPath $ProjectDir -Filter *.Art.xml -File -ErrorAction SilentlyContinue)
  if ($artXmls.Count -gt 1) {
    throw "There is more than one .Art.xml file in the project root; exactly one is allowed."
  }

  $hasArt = $artXmls.Count -eq 1
  $artXml = $null
  if ($hasArt) { $artXml = $artXmls[0].FullName }

  $artDefFiles = @()
  $artDefDir = Join-Path $ProjectDir 'ArtDefs'
  if (Test-Path -LiteralPath $artDefDir -PathType Container) {
    $artDefFiles = @(Get-ChildItem -LiteralPath $artDefDir -Filter *.artdef -File -ErrorAction SilentlyContinue | Sort-Object Name)
  }

  $xlpFiles = @()
  $xlpDir = Join-Path $ProjectDir 'XLPs'
  if (Test-Path -LiteralPath $xlpDir -PathType Container) {
    $xlpFiles = @(Get-ChildItem -LiteralPath $xlpDir -Filter *.xlp -File -ErrorAction SilentlyContinue | Sort-Object Name)
  }

  return [pscustomobject]@{ HasArt = $hasArt; ArtXml = $artXml; ArtDefs = $artDefFiles; Xlps = $xlpFiles }
}

function Assert-CookerReady {
  param($Ctx)

  $cookerDir = Join-Path (Join-Path $Ctx.BaseSdk 'AssetModTools') 'Cooker'
  $exe = Join-Path $cookerDir 'Civ6AssetCooker_FinalRelease.exe'
  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Cannot find the asset cooker at '$exe'. The mod includes art assets, so the cooker is required."
  }
  if (-not (Test-Path -LiteralPath $Ctx.BaseAssets -PathType Container)) {
    throw "Base assets path not found: $($Ctx.BaseAssets)"
  }
}

function Invoke-CookerCommand {
  param([string]$Exe, [string[]]$Argv, [string]$WorkingDirectory, [string]$Description)

  $code = 0
  Push-Location -LiteralPath $WorkingDirectory
  try {
    & $Exe @Argv | Out-Host
    $code = $LASTEXITCODE
  }
  catch {
    throw "Failed to launch the asset cooker for ${Description}: $($_.Exception.Message)"
  }
  finally {
    Pop-Location
  }

  if ($code -ne 0) {
    throw "The asset cooker failed for $Description (exit code $code)."
  }
}

function Invoke-Cooker {
  param($Ctx, $ArtInfo)

  $cookerDir = Join-Path (Join-Path $Ctx.BaseSdk 'AssetModTools') 'Cooker'
  $exe = Join-Path $cookerDir 'Civ6AssetCooker_FinalRelease.exe'
  $cfg = Join-Path $cookerDir 'Civ6.cfg'

  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Cannot find the asset cooker at '$exe'. The mod includes art assets, so the cooker is required."
  }

  $rootInfo = Read-ArtXmlInfo $ArtInfo.ArtXml
  $pantries = @(Resolve-PantryPaths -ProjectDir $Ctx.ProjectDir -BaseAssets $Ctx.BaseAssets -RootRequired $rootInfo.Required)

  $baseArgs = @('--absolute_paths', '--no_mt')
  $pantryArgs = @('--pantry', $Ctx.ProjectDir) + $pantries
  $banquet = Join-Path $Ctx.Out 'ArtDefs'
  $stewpotWindows = Join-Path (Join-Path (Join-Path $Ctx.Out 'Platforms') 'Windows') 'BLPs'
  $stewpotMacOS = Join-Path (Join-Path (Join-Path $Ctx.Out 'Platforms') 'MacOS') 'BLPs'

  $cooks = 0

  foreach ($file in $ArtInfo.ArtDefs) {
    $argv = $baseArgs + @('--mode', 'ArtDef', '--platform', 'Windows', '--shaders', $cookerDir) + $pantryArgs + @('--banquet_hall', $banquet, '--config', $cfg, $file.FullName)
    Invoke-CookerCommand -Exe $exe -Argv $argv -WorkingDirectory $Ctx.ProjectDir -Description ("ArtDef " + $file.Name)
    $cooks++
  }

  foreach ($file in $ArtInfo.Xlps) {
    $argv = $baseArgs + @('--mode', 'XLP', '--platform', 'Windows', '--shaders', $cookerDir) + $pantryArgs + @('--stewpot', $stewpotWindows, '--config', $cfg, $file.FullName)
    Invoke-CookerCommand -Exe $exe -Argv $argv -WorkingDirectory $Ctx.ProjectDir -Description ("XLP " + $file.Name + " (Windows)")
    $cooks++

    $argv = $baseArgs + @('--mode', 'XLP', '--platform', 'MacOS', '--shaders', $cookerDir) + $pantryArgs + @('--stewpot', $stewpotMacOS, '--config', $cfg, $file.FullName)
    Invoke-CookerCommand -Exe $exe -Argv $argv -WorkingDirectory $Ctx.ProjectDir -Description ("XLP " + $file.Name + " (MacOS)")
    $cooks++
  }

  return $cooks
}

function Resolve-DepPath {
  param([string]$Out, [string]$ProjectDir)

  if (-not (Test-Path -LiteralPath $Out -PathType Container)) {
    throw "Output directory not found: $Out"
  }

  $deps = @(Get-ChildItem -LiteralPath $Out -Recurse -Filter *.dep -File -ErrorAction SilentlyContinue)

  if ($deps.Count -eq 0) {
    $strays = @(Get-ChildItem -LiteralPath $ProjectDir -Filter *.dep -File -ErrorAction SilentlyContinue)
    if ($strays.Count -eq 1) {
      $dest = Join-Path $Out $strays[0].Name
      Move-Item -LiteralPath $strays[0].FullName -Destination $dest -Force -ErrorAction Stop
      Write-Warning "The asset cooker wrote '$($strays[0].Name)' into the project directory; moved it into the output directory."
      $deps = @(Get-Item -LiteralPath $dest)
    }
  }

  if ($deps.Count -eq 0) {
    throw "No '.dep' art dependency file was found in '$Out'. Run a full build (without --skip-art) first."
  }
  if ($deps.Count -gt 1) {
    throw "More than one '.dep' file was found under '$Out'; cannot determine the art dependency file."
  }

  $rel = $deps[0].FullName.Substring($Out.Length).TrimStart('\', '/')
  return ($rel -replace '\\', '/')
}

function ConvertFrom-XmlFragment {
  param([string]$Xml)
  $doc = New-Object System.Xml.XmlDocument
  try {
    $doc.LoadXml($Xml)
  }
  catch {
    throw "Failed to parse project data fragment: $($_.Exception.Message)"
  }
  return $doc.DocumentElement
}

function Add-TextElement {
  param($Document, $Parent, [string]$Name, [string]$Value)
  $el = $Document.CreateElement($Name)
  $el.InnerText = $Value
  $Parent.AppendChild($el) | Out-Null
}

function New-ModInfo {
  param($Ctx, $Project, [string]$DepRelativePath, $CopiedFiles)

  $modDoc = New-Object System.Xml.XmlDocument
  $decl = $modDoc.CreateXmlDeclaration('1.0', 'utf-8', $null)
  $modDoc.AppendChild($decl) | Out-Null

  $root = $modDoc.CreateElement('Mod')
  $root.SetAttribute('id', $Project.Guid)
  $root.SetAttribute('version', $Project.ModVersion)
  $modDoc.AppendChild($root) | Out-Null

  $props = $modDoc.CreateElement('Properties')
  Add-TextElement -Document $modDoc -Parent $props -Name 'Name' -Value $Project.Name
  Add-TextElement -Document $modDoc -Parent $props -Name 'Description' -Value $Project.Description
  Add-TextElement -Document $modDoc -Parent $props -Name 'Created' -Value ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds().ToString())
  Add-TextElement -Document $modDoc -Parent $props -Name 'Teaser' -Value $Project.Teaser
  Add-TextElement -Document $modDoc -Parent $props -Name 'Authors' -Value $Project.Authors
  Add-TextElement -Document $modDoc -Parent $props -Name 'CompatibleVersions' -Value $Project.CompatibleVersions
  if (-not [string]::IsNullOrEmpty($Project.SpecialThanks)) {
    Add-TextElement -Document $modDoc -Parent $props -Name 'SpecialThanks' -Value $Project.SpecialThanks
  }
  if (-not [string]::IsNullOrEmpty($Project.Homepage)) {
    Add-TextElement -Document $modDoc -Parent $props -Name 'Homepage' -Value $Project.Homepage
  }
  $root.AppendChild($props) | Out-Null

  if (-not [string]::IsNullOrEmpty($Project.AssociationData)) {
    $assocDoc = New-Object System.Xml.XmlDocument
    $assocDoc.LoadXml($Project.AssociationData)
    $blocks = $modDoc.CreateElement('Blocks')
    foreach ($node in $assocDoc.DocumentElement.ChildNodes) {
      if ($node.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
      if ($node.Name -ne 'Block') { continue }
      $blockMod = $modDoc.CreateElement('Mod')
      $blockMod.SetAttribute('id', $node.GetAttribute('id'))
      $blockMod.SetAttribute('title', $node.GetAttribute('title'))
      $blocks.AppendChild($blockMod) | Out-Null
    }
    if ($blocks.HasChildNodes) { $root.AppendChild($blocks) | Out-Null }
  }

  if (-not [string]::IsNullOrEmpty($Project.ActionCriteriaData)) {
    $root.AppendChild($modDoc.ImportNode((ConvertFrom-XmlFragment $Project.ActionCriteriaData), $true)) | Out-Null
  }

  $frontEnd = $null
  if (-not [string]::IsNullOrEmpty($Project.FrontEndActionData)) {
    $frontEnd = $modDoc.ImportNode((ConvertFrom-XmlFragment $Project.FrontEndActionData), $true)
    $root.AppendChild($frontEnd) | Out-Null
  }

  $inGame = $null
  if (-not [string]::IsNullOrEmpty($Project.InGameActionData)) {
    $inGame = $modDoc.ImportNode((ConvertFrom-XmlFragment $Project.InGameActionData), $true)
    $root.AppendChild($inGame) | Out-Null
  }

  foreach ($container in @($frontEnd, $inGame)) {
    if ($null -eq $container) { continue }
    foreach ($file in $container.SelectNodes('.//File')) {
      if ($file.InnerText -eq '(Mod Art Dependency File)') {
        $file.InnerText = $DepRelativePath
      }
    }
  }

  if (-not [string]::IsNullOrEmpty($Project.LocalizedTextData)) {
    $root.AppendChild($modDoc.ImportNode((ConvertFrom-XmlFragment $Project.LocalizedTextData), $true)) | Out-Null
  }

  $modInfoPath = Join-Path $Ctx.Out ($Ctx.ProjectName + '.modinfo')

  $expected = New-Object System.Collections.Generic.List[string]
  foreach ($rel in $CopiedFiles) { $expected.Add($rel) }

  foreach ($f in @(Get-ChildItem -LiteralPath $Ctx.Out -File -Filter *.dep -ErrorAction SilentlyContinue)) {
    $expected.Add((($f.FullName.Substring($Ctx.Out.Length).TrimStart('\', '/')) -replace '\\', '/'))
  }

  foreach ($sub in @('ArtDefs', 'Platforms')) {
    $dir = Join-Path $Ctx.Out $sub
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue)) {
      $expected.Add((($f.FullName.Substring($Ctx.Out.Length).TrimStart('\', '/')) -replace '\\', '/'))
    }
  }

  $filesEl = $modDoc.CreateElement('Files')
  foreach ($rel in ($expected | Sort-Object -Unique)) {
    $fileEl = $modDoc.CreateElement('File')
    $fileEl.InnerText = $rel
    $filesEl.AppendChild($fileEl) | Out-Null
  }
  $root.AppendChild($filesEl) | Out-Null

  $settings = New-Object System.Xml.XmlWriterSettings
  $settings.Indent = $true
  $settings.IndentChars = '  '
  $settings.Encoding = New-Object System.Text.UTF8Encoding($true)

  $writer = [System.Xml.XmlWriter]::Create($modInfoPath, $settings)
  try {
    $modDoc.Save($writer)
  }
  finally {
    $writer.Dispose()
  }

  return $modInfoPath
}

function Main {
  param([string[]]$ArgList)

  $opts = Parse-Args -ArgList $ArgList

  if ($opts.Help) {
    [Console]::Out.WriteLine((Get-UsageText))
    return 0
  }

  if ([string]::IsNullOrEmpty($opts.Input)) {
    [Console]::Error.WriteLine((Get-UsageText))
    return 1
  }

  $ctx = Assert-Inputs -opts $opts

  $project = Read-Project -Path $ctx.InputPath
  $artInfo = Get-ArtInfo -ProjectDir $ctx.ProjectDir
  $willCook = (-not $ctx.SkipArt) -and $artInfo.HasArt -and ($artInfo.ArtDefs.Count -gt 0 -or $artInfo.Xlps.Count -gt 0)
  if ($willCook) { Assert-CookerReady -Ctx $ctx }

  if ($ctx.Clean) {
    Invoke-Clean -Out $ctx.Out -ProjectDir $ctx.ProjectDir -DefaultOut $ctx.DefaultOut
  }
  if ($ctx.Clean -or $ctx.ForceDll) {
    Remove-DllIntermediate -BuildDir $ctx.DllBuildDir -ProjectDir $ctx.ProjectDir
  }
  if ($ctx.ForceArt) {
    Remove-CookedArt -Out $ctx.Out -ProjectDir $ctx.ProjectDir -DefaultOut $ctx.DefaultOut
    Write-Host "Forced art rebuild: cleared cooked art and .dep."
  }

  $copyResult = Copy-ContentFiles -ContentList $project.Content -ProjectDir $ctx.ProjectDir -Out $ctx.Out -SkipArt $ctx.SkipArt

  $dllFiles = @()
  if (-not $ctx.SkipDll) {
    Invoke-DllBuild -CMake $ctx.CMake -DllDir $ctx.DllDir -BuildDir $ctx.DllBuildDir -Config $ctx.DllConfig -Generator $ctx.DllGenerator -ToolchainFile $ctx.VcpkgToolchain -Triplet $ctx.VcpkgTriplet -CMakeArgs $ctx.CMakeArgs
    $dllFiles = @(Copy-DllArtifacts -Ctx $ctx)
  }
  else {
    $dllFiles = @(Get-DeployedDllFiles -Ctx $ctx)
    $expectedDlls = $PluginDlls.Count
    if (-not [string]::IsNullOrWhiteSpace($LoaderBuildDll)) { $expectedDlls += 1 }
    if ($dllFiles.Count -lt $expectedDlls) {
      throw "Option '--skip=dll' was given, but the expected DLLs were not all found in '$($ctx.Out)'. Build once without '--skip=dll' first."
    }
  }

  $cooks = 0
  if ($willCook) {
    $cooks = Invoke-Cooker -Ctx $ctx -ArtInfo $artInfo
  }

  $depRel = $null
  if ($artInfo.HasArt) {
    $depRel = Resolve-DepPath -Out $ctx.Out -ProjectDir $ctx.ProjectDir
  }
  $allFiles = @($copyResult.Files) + @($dllFiles)
  $modInfoPath = New-ModInfo -Ctx $ctx -Project $project -DepRelativePath $depRel -CopiedFiles $allFiles

  Write-Host ("Copied {0} content file(s)." -f $copyResult.Count)
  if ($ctx.SkipDll) {
    Write-Host ("Reused {0} existing DLL file(s)." -f $dllFiles.Count)
  }
  else {
    Write-Host ("Deployed {0} DLL file(s)." -f $dllFiles.Count)
  }
  Write-Host ("Ran {0} asset cook(s)." -f $cooks)
  Write-Host ("Wrote {0}" -f $modInfoPath)
  return 0
}

$finalExitCode = 0
try {
  $finalExitCode = Main -ArgList $args
}
catch {
  [Console]::Error.WriteLine("Error: " + $_.Exception.Message)
  $finalExitCode = 1
}
exit $finalExitCode
