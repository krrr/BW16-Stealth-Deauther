export interface ApAdvancedInfo {
  uptime?: number
  pmfCapable?: boolean
  pmfRequired?: boolean
}

export interface NetworkInfo {
  ssid: string
  bssid: string
  rssi: number
  channel: number
  security: string
  lastSeen?: string
  band?: string
  advanced_info?: ApAdvancedInfo
}

export interface DeviceInfo {
  mac: string
  packets_out: number
  packets_in: number
  handshakes: number
  rssi: number
  lastSeen?: string
}

export const isVirtualMac = (mac: string): boolean => {
  if (!mac) return false
  const cleanMac = mac.replace(/[^0-9a-fA-F]/g, '')
  if (cleanMac.length < 2) return false
  const firstByte = parseInt(cleanMac.substring(0, 2), 16)
  return !isNaN(firstByte) && (firstByte & 2) !== 0
}

export const formatUptime = (seconds?: number): string => {
  if (seconds === undefined) return ''
  const days = Math.floor(seconds / 86400)
  const hours = Math.floor((seconds % 86400) / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)

  const parts: string[] = []
  if (days > 0) parts.push(`${days}d`)
  if (hours > 0) parts.push(`${hours}h`)
  if (parts.length === 0 || minutes > 0) parts.push(`${minutes}m`)
  return parts.join('')
}

const escapeTableCell = (str?: string): string => {
  if (!str) return ''
  return str.replace(/\|/g, '\\|')
}

/**
 * Generate Markdown report for all scanned APs and associated devices
 */
export const generateScanMarkdown = (
  scanResults: NetworkInfo[],
  deviceResults: Record<string, DeviceInfo[]>
): string => {
  const nowStr = new Date().toLocaleString('sv-SE')
  let totalDevices = 0
  for (const ap of scanResults) {
    const devs = deviceResults[ap.bssid]
    if (devs) {
      totalDevices += devs.length
    }
  }

  const lines: string[] = []

  // Title and metadata
  lines.push('# WiFi Scan Report')
  lines.push('')
  lines.push(`- **Export Time**: ${nowStr}`)
  lines.push(`- **Total Access Points**: ${scanResults.length}`)
  lines.push(`- **Total Discovered Devices**: ${totalDevices}`)
  lines.push('')
  lines.push('---')
  lines.push('')

  // Overview Table
  lines.push('## Access Points Overview')
  lines.push('')
  lines.push('| SSID | BSSID | RSSI | Channel | Band | Security | Devices | Last Seen |')
  lines.push('| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |')

  for (const ap of scanResults) {
    const ssidDisplay = escapeTableCell(ap.ssid) || '(Hidden)'
    const isVirtual = isVirtualMac(ap.bssid)
    const bssidDisplay = `\`${ap.bssid}\`${isVirtual ? ' (Virtual)' : ''}`
    const rssiDisplay = ap.rssi ? `${ap.rssi} dBm` : '—'
    const devCount = deviceResults[ap.bssid]?.length || 0
    const bandDisplay = ap.band || (ap.channel >= 36 ? '5G' : '2.4G')
    const lastSeenDisplay = ap.lastSeen || '—'

    lines.push(
      `| ${ssidDisplay} | ${bssidDisplay} | ${rssiDisplay} | ${ap.channel} | ${bandDisplay} | ${escapeTableCell(ap.security)} | ${devCount} | ${lastSeenDisplay} |`
    )
  }

  lines.push('')
  lines.push('---')
  lines.push('')

  // Detailed AP and Devices Breakdown
  lines.push('## Detailed AP & Device Breakdown')
  lines.push('')

  scanResults.forEach((ap, index) => {
    const ssidDisplay = ap.ssid || '(Hidden)'
    const isVirtual = isVirtualMac(ap.bssid)
    const bandDisplay = ap.band || (ap.channel >= 36 ? '5G' : '2.4G')
    const devs = deviceResults[ap.bssid] || []

    lines.push(`### ${index + 1}. ${ssidDisplay} (\`${ap.bssid}\`${isVirtual ? ' (Virtual)' : ''})`)
    lines.push(`- **Signal (RSSI)**: ${ap.rssi ? `${ap.rssi} dBm` : '—'}`)
    lines.push(`- **Channel / Band**: Ch ${ap.channel} (${bandDisplay})`)
    lines.push(`- **Security**: ${ap.security}`)
    if (ap.lastSeen) {
      lines.push(`- **Last Seen**: ${ap.lastSeen}`)
    }
    if (ap.advanced_info) {
      if (ap.advanced_info.uptime !== undefined) {
        lines.push(`- **Wireless Uptime**: ${formatUptime(ap.advanced_info.uptime)}`)
      }
      if (ap.advanced_info.pmfRequired) {
        lines.push('- **PMF (Protected Management Frames)**: Required')
      } else if (ap.advanced_info.pmfCapable) {
        lines.push('- **PMF (Protected Management Frames)**: Capable')
      } else {
        lines.push('- **PMF (Protected Management Frames)**: Disabled')
      }
    }

    lines.push('')
    if (devs.length > 0) {
      lines.push(`#### Associated Devices (${devs.length})`)
      lines.push('')
      lines.push('| MAC | RSSI | Packets (TX/RX) | Handshakes | Last Seen |')
      lines.push('| :--- | :--- | :--- | :--- | :--- |')
      for (const dev of devs) {
        const isDevVirtual = isVirtualMac(dev.mac)
        const devMacDisplay = `\`${dev.mac}\`${isDevVirtual ? ' (Virtual)' : ''}`
        const devRssi = dev.rssi ? `${dev.rssi} dBm` : '—'
        const packets = `TX: ${dev.packets_out || 0} / RX: ${dev.packets_in || 0}`
        const handshakes = dev.handshakes || 0
        const devLastSeen = dev.lastSeen || '—'
        lines.push(`| ${devMacDisplay} | ${devRssi} | ${packets} | ${handshakes} | ${devLastSeen} |`)
      }
    } else {
      lines.push('*No associated devices discovered.*')
    }
    lines.push('')
  })

  return lines.join('\n')
}

/**
 * Trigger download of markdown text as a file
 */
export const downloadMarkdown = (content: string, filename?: string): void => {
  const now = new Date()
  const pad = (n: number) => String(n).padStart(2, '0')
  const defaultName = `wifi_scan_${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}_${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}.md`
  const finalName = filename || defaultName

  const blob = new Blob([content], { type: 'text/markdown;charset=utf-8;' })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.setAttribute('download', finalName)
  document.body.appendChild(link)
  link.click()
  document.body.removeChild(link)
  URL.revokeObjectURL(url)
}
