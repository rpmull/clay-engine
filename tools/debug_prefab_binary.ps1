param(
    [string]$PrefabPath = "C:\Users\rpmul\projects\alkahest\claymore\.bin\prefabs\assets_prefabs_EqItemSlot_UI.prefabb"
)

$bytes = [System.IO.File]::ReadAllBytes($PrefabPath)
Write-Host "=== Prefab Binary Analysis ===" -ForegroundColor Cyan
Write-Host "File size: $($bytes.Length) bytes" -ForegroundColor Green

if ($bytes.Length -lt 32) {
    Write-Host "File too small!" -ForegroundColor Red
    exit 1
}

# Read header
$magic = [BitConverter]::ToUInt32($bytes, 0)
$version = [BitConverter]::ToUInt32($bytes, 4)
$flags = [BitConverter]::ToUInt32($bytes, 8)
$entityCount = [BitConverter]::ToUInt32($bytes, 24)
$rootEntityIndex = [BitConverter]::ToUInt32($bytes, 28)
$componentTableOffset = [BitConverter]::ToUInt32($bytes, 32)
$stringTableOffset = [BitConverter]::ToUInt32($bytes, 36)

Write-Host "Magic: 0x$($magic.ToString('X8'))" -ForegroundColor $(if ($magic -eq 0x42524650) { "Green" } else { "Red" })
Write-Host "Version: $version"
Write-Host "Flags: 0x$($flags.ToString('X8'))"
Write-Host "Entity count: $entityCount"
Write-Host "Root entity index: $rootEntityIndex"
Write-Host "Component table offset: $componentTableOffset"
Write-Host "String table offset: $stringTableOffset"
Write-Host ""

# Component type IDs
$ComponentTypeId = @{
    0 = "None"
    1 = "Transform"
    16 = "Panel"
    17 = "Button"
    18 = "Text"
    15 = "Canvas"
}

# Entity header is 64 bytes (v4 format)
$headerSize = 32  # PrefabBinaryHeader base
$prefabGuidSize = 16
$prefabNameSize = 4
$entityTableOffset = $headerSize + $prefabGuidSize + $prefabNameSize

Write-Host "=== Entity Headers ===" -ForegroundColor Cyan
for ($i = 0; $i -lt $entityCount; $i++) {
    $offset = $entityTableOffset + ($i * 64)  # EntityHeader is 64 bytes
    
    if ($offset + 64 -gt $bytes.Length) {
        Write-Host "Entity $i header out of bounds" -ForegroundColor Red
        break
    }
    
    # Read EntityHeader (simplified - just get componentCount and componentOffset)
    $componentCount = [BitConverter]::ToUInt32($bytes, $offset + 56)
    $componentOffset = [BitConverter]::ToUInt32($bytes, $offset + 60)
    $nameIndex = [BitConverter]::ToUInt32($bytes, $offset + 20)
    
    Write-Host "Entity ${i}:" -ForegroundColor Yellow
    Write-Host "  Name index: $nameIndex"
    Write-Host "  Component count: $componentCount"
    Write-Host "  Component offset: $componentOffset"
    
    # Read components
    if ($componentCount -gt 0 -and $componentOffset -lt $bytes.Length) {
        Write-Host "  Components:" -ForegroundColor White
        $compBaseOffset = $componentTableOffset + $componentOffset
        
        for ($c = 0; $c -lt $componentCount; $c++) {
            $compEntryOffset = $compBaseOffset + ($c * 12)  # ComponentEntry is 12 bytes
            
            if ($compEntryOffset + 12 -gt $bytes.Length) {
                Write-Host "    Component entry $c out of bounds" -ForegroundColor Red
                break
            }
            
            # Read ComponentEntry
            $typeId = [BitConverter]::ToUInt16($bytes, $compEntryOffset)
            $dataSize = [BitConverter]::ToUInt32($bytes, $compEntryOffset + 4)
            $dataOffset = [BitConverter]::ToUInt32($bytes, $compEntryOffset + 8)
            
            $typeName = if ($ComponentTypeId.ContainsKey($typeId)) { $ComponentTypeId[$typeId] } else { "Type$typeId" }
            
            $color = if ($typeId -eq 16) { "Green" } elseif ($typeId -eq 17) { "Cyan" } else { "White" }
            Write-Host "    [$c] $typeName (ID=$typeId, Size=$dataSize, DataOffset=$dataOffset)" -ForegroundColor $color
            
            if ($typeId -eq 16) {  # Panel
                $panelDataOffset = $componentTableOffset + $componentOffset + $dataOffset
                Write-Host "      *** PANEL FOUND ***" -ForegroundColor Green -BackgroundColor Black
                Write-Host "      Panel data absolute offset: $panelDataOffset" -ForegroundColor Green
                Write-Host "      Panel data size: $dataSize bytes" -ForegroundColor Green
                
                if ($panelDataOffset + $dataSize -gt $bytes.Length) {
                    Write-Host "      ERROR: Panel data out of bounds!" -ForegroundColor Red -BackgroundColor Yellow
                } else {
                    Write-Host "      Panel data is within bounds" -ForegroundColor Green
                }
            }
        }
    }
    Write-Host ""
}
