<template>
  <section>
    <article>
      <header>
        <img src="../assets/radio.svg" />
        <h1>WiFi Scan</h1>
      </header>
      <div style="display:flex;gap:0.75rem;align-items:center;flex-wrap:wrap;">
        <button @click="startScan" :aria-busy="scanning" :disabled="scanning" class="contrast" style="min-width:9rem;">
          <span>{{ scanning ? 'Scanning...' : 'Start Scan' }}</span>
        </button>
        <button @click="clearAllPersisted" class="outline secondary" title="Clear Scanned Data" style="margin-left: auto; padding: 0.45rem 0.75rem; display: inline-flex; align-items: center; justify-content: center;">
          <img src="../assets/clean.svg" width="20" height="20" alt="Clear" />
        </button>
      </div>

      <p v-if="scanError" role="alert" class="error">{{ scanError }}</p>

      <div v-if="scanResults.length > 0" style="overflow-x:auto;">
        <table>
          <thead>
            <tr>
              <th title="Service Set Identifier">SSID</th>
              <th title="Basic Service Set Identifier (BSSID)">MAC</th>
              <th title="Received Signal Strength Indicator">RSSI</th>
              <th>Ch</th>
              <th>Band</th>
              <th>Security</th>
              <th>Last Seen</th>
              <th>Operations</th>
            </tr>
          </thead>
          <template v-for="ap in scanResults" :key="ap.bssid">
            <tbody>
              <tr>
                <td>{{ ap.ssid || '(Hidden)' }}</td>
                <td class="smaller-td">
                  <span style="font-family:monospace;cursor:pointer" @click="openMacLookup(ap.bssid)">
                    {{ ap.bssid }}
                  </span>
                  <span v-if="isVirtualMac(ap.bssid)" class="badge badge-red" style="font-size: 0.7em;">Virtual</span>
                </td>
                <td :style="{ color: rssiColor(ap.rssi), fontWeight: 'bold' }">{{ ap.rssi }} <span class="dbm-unit">dBm</span></td>
                <td>{{ ap.channel }}</td>
                <td>{{ ap.band }}</td>
                <td class="smaller-td">{{ ap.security }}</td>
                <td class="smaller-td">{{ ap.lastSeen }}</td>
                <td class="btn-col">
                  <button
                    @click="deviceScanning === ap.bssid ? stopDeviceScan(ap.bssid) : startDeviceScan(ap.bssid, ap.channel)"
                    :aria-busy="deviceScanning === ap.bssid"
                    :class="{ 'pointer-ev-auto': deviceScanning === ap.bssid }"
                    :disabled="deviceScanning !== null && deviceScanning !== ap.bssid"
                    class="outline contrast btn-sm"
                  >
                    <span>{{ deviceScanning === ap.bssid ? 'Stop Listening' : 'Scan Devices' }}</span>
                  </button>
                  <Dropdown summary-class="outline secondary btn-sm" align="right">
                    <li>
                      <a
                        :class="{ 'disabled-link': deviceScanning !== null }"
                        @click.prevent="deviceScanning === null && startDeviceScan(ap.bssid, ap.channel, -1)"
                      >
                        Scan Devices Nonstop
                      </a>
                    </li>
                    <li>
                      <a @click.prevent="clearDevices(ap.bssid)">
                        Clear Results
                      </a>
                    </li>
                  </Dropdown>
                </td>
              </tr>
              <!-- Device scan results for this AP -->
              <tr v-if="deviceResults[ap.bssid]?.length > 0 || ap.advanced_info">
                <td colspan="8" style="padding:0;background:var(--card-background-color, #f8f9fa);">
                  <div class="dev-table">
                    <div class="dev-table-header" @click="toggleExpanded(ap.bssid)" >
                      <div :title="isExpanded(ap.bssid) ? 'Collapse' : 'Expand'" style="display: inline-flex; align-items: center; gap: 0.25rem;">
                        <img src="../assets/chevron-up.svg" width="21" height="21"
                          style="transition: transform 0.2s;"
                          :style="isExpanded(ap.bssid) ? 'transform: rotate(180deg);' : 'transform: rotate(90deg);'"
                        />
                        <small>Discovered Devices (<span>{{ deviceResults[ap.bssid]?.length || 0 }}</span>)</small>
                      </div>
                      
                      <!-- Middle Area for AP Advanced Properties -->
                      <div v-if="ap.advanced_info" class="adv-ap-inf">
                        <span v-if="ap.advanced_info.uptime !== undefined" title="Wireless Uptime" style="display:inline-flex;align-items:center;gap:2px;">
                          <img src="../assets/hourglass.svg" width="20" height="20" />
                          Uptime: <i>{{ formatUptime(ap.advanced_info.uptime) }}</i>
                        </span>
                        <span title="PMF (Protected Management Frames)" style="display:inline-flex;align-items:center;gap:2px;">
                          <img src="../assets/protection.svg" width="20" height="20" /> PMF: 
                          <i v-if="ap.advanced_info.pmfRequired">Required</i>
                          <i v-else-if="ap.advanced_info.pmfCapable" >Capable</i>
                          <i v-else style="color:var(--muted-color)">Disabled</i>
                        </span>
                      </div>
                    </div>
                    <div v-show="isExpanded(ap.bssid) && deviceResults[ap.bssid]?.length > 0" class="dev-table-table">
                      <table style="font-size:0.85em;">
                        <thead>
                          <tr>
                            <th>MAC</th>
                            <th>RSSI</th>
                            <th>Packets</th>
                            <th>Handshakes</th>
                            <th>Last Seen</th>
                            <th>Operations</th>
                          </tr>
                        </thead>
                        <tbody>
                          <tr v-for="dev in deviceResults[ap.bssid]" :key="dev.mac">
                            <td>
                              <span style="font-family:monospace;cursor:pointer" @click="openMacLookup(dev.mac)">
                                {{ dev.mac }}
                              </span>
                              <span v-if="isVirtualMac(dev.mac)" class="badge badge-red">Virtual</span>
                            </td>
                            <td>
                              <span v-if="dev.rssi" :style="{ color: rssiColor(dev.rssi), fontWeight: 'bold' }">{{ dev.rssi }} <span class="dbm-unit">dBm</span></span>
                              <span v-else style="color:var(--muted-color)">—</span>
                            </td>
                            <td>{{ '↑' + (dev.packets_out || 0) + ' / ↓' + (dev.packets_in || 0) }}</td>
                            <td>{{ dev.handshakes || 0 }}</td>
                            <td>{{ dev.lastSeen }}</td>
                            <td>
                              <button
                                v-if="!isTargeted(ap.bssid, dev.mac)"
                                @click="addToAttack(ap, dev)"
                                title="Add to attack schedule"
                                class="outline btn-sm"
                                style="margin-right:0.25rem;"
                              >
                                Add
                              </button>
                              <button
                                v-else
                                disabled
                                title="Already in attack schedule, can be removed in attack page"
                                class="outline btn-sm"
                                style="margin-right:0.25rem;color:var(--muted-color);border-color:var(--muted-color);"
                              >
                                ✓ Added
                              </button>
                              <button
                                @click="deauthDevice(ap.bssid, dev.mac, ap.channel)"
                                :aria-busy="!!deauthing[ap.bssid + '-' + dev.mac]"
                                :class="{ 'pointer-ev-auto': !!deauthing[ap.bssid + '-' + dev.mac] }"
                                :disabled="activeDeauthKey !== null && activeDeauthKey !== (ap.bssid + '-' + dev.mac)"
                                class="outline contrast btn-sm"
                                style="color:#e74c3c;border-color:#e74c3c;"
                              >
                                <span>{{ deauthing[ap.bssid + '-' + dev.mac] ? 'Stop Listening' : 'Deauth' }}</span>
                              </button>
                            </td>
                          </tr>
                        </tbody>
                      </table>
                    </div>
                  </div>
                </td>
              </tr>
              <!-- Error row -->
              <tr v-if="deviceErrors[ap.bssid]">
                <td colspan="8" style="padding:0.25rem 1rem;color:var(--del-color,#c0392b);font-size:0.85em;">
                  <span>{{ deviceErrors[ap.bssid] }}</span>
                </td>
              </tr>
            </tbody>
          </template>
        </table>
      </div>

      <footer>
        <small v-if="scanResultCount !== null">Discovered {{ scanResultCount }} networks</small>
        <small v-else>Scan takes approx. 10 seconds</small>
      </footer>
    </article>
  </section>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import Dropdown from '../components/Dropdown.vue'
import { message } from '../utils/message'
import { addTarget, loadPlan, type AttackTarget } from '../utils/attackPlan'

interface ApAdvancedInfo {
  uptime?: number
  pmfCapable?: boolean
  pmfRequired?: boolean
}

interface NetworkInfo {
  ssid: string
  bssid: string
  rssi: number
  channel: number
  security: string
  lastSeen?: string
  band?: string
  advanced_info?: ApAdvancedInfo
}

interface ScanResponse {
  success: boolean
  message?: string
  networks: NetworkInfo[]
}

interface DeviceInfo {
  mac: string
  packets_out: number
  packets_in: number
  handshakes: number
  rssi: number
  lastSeen?: string
}

interface DeviceScanResponse {
  success: boolean
  message?: string
  bssid: string
  channel: number
  count: number
  devices: DeviceInfo[]
  ap_beacon_parsed?: boolean
  ap_uptime?: number
  pmf_capable?: boolean
  pmf_required?: boolean
  ap_rssi?: number
}

interface TestDeauthResponse {
  success: boolean
  rounds: number
  packets: number
  detected: boolean
  handshake_delay_ms: number
}

const STORAGE_KEY = 'wifi_scan_data'

const scanning = ref(false)
const scanError = ref('')
const scanResults = ref<NetworkInfo[]>([])
const scanResultCount = ref<number | null>(null)

const deviceScanning = ref<string | null>(null)
const deviceErrors = ref<Record<string, string>>({})
const deviceResults = ref<Record<string, DeviceInfo[]>>({})
const deviceEventSource = ref<EventSource | null>(null)

const deauthing = ref<Record<string, boolean>>({})

// 攻击计划草稿目标（sessionStorage）：设备 -> "已加入" 标记
const targetedKeys = ref<Record<string, boolean>>({})
const targetedKey = (bssid: string, mac: string) => mac + '|' + bssid
const isTargeted = (bssid: string, mac: string) => targetedKeys.value[targetedKey(bssid, mac)] === true

const addToAttack = (ap: NetworkInfo, dev: DeviceInfo) => {
  if (isTargeted(ap.bssid, dev.mac)) return
  const t: AttackTarget = {
    mac: dev.mac,
    bssid: ap.bssid,
    ssid: ap.ssid || '(Hidden)',
    channel: ap.channel,
    rssi: dev.rssi || 0,
    lastSeen: dev.lastSeen,
  }
  addTarget(t)
  targetedKeys.value[targetedKey(ap.bssid, dev.mac)] = true
  message.success(`Added to attack plan: ${dev.mac}`)
}

const expandedBssids = ref<Record<string, boolean>>({})

const isExpanded = (bssid: string) => {
  return expandedBssids.value[bssid] === true
}

const toggleExpanded = (bssid: string) => {
  expandedBssids.value[bssid] = !isExpanded(bssid)
}

const loadPersisted = () => {
  const saved = localStorage.getItem(STORAGE_KEY)
  if (!saved) return
  try {
    const data = JSON.parse(saved)
    if (data.scanResults) scanResults.value = data.scanResults
    if (data.deviceResults) deviceResults.value = data.deviceResults
    if (scanResults.value.length > 0) scanResultCount.value = scanResults.value.length
  } catch { /* ignore corrupt data */ }
}

const savePersisted = () => {
  localStorage.setItem(STORAGE_KEY, JSON.stringify({
    scanResults: scanResults.value,
    deviceResults: deviceResults.value,
  }))
}

const clearAllPersisted = () => {
  if (deviceScanning.value) {
    stopDeviceScan(deviceScanning.value)
  }
  stopDeauth()
  scanResults.value = []
  deviceResults.value = {}
  deviceErrors.value = {}
  scanResultCount.value = null
  expandedBssids.value = {}
  localStorage.removeItem(STORAGE_KEY)
  message.success('Data cleared in the browser')
}

const startScan = async () => {
  scanning.value = true
  scanError.value = ''
  scanResultCount.value = null
  try {
    const r = await fetch('/api/scan')
    const data: ScanResponse = await r.json()
    if (data.success) {
      const now = new Date().toLocaleString('sv-SE').slice(5)
      for (const ap of data.networks) {
        const existing = scanResults.value.find(a => a.bssid === ap.bssid)
        if (existing) {
          Object.assign(existing, ap, { lastSeen: now })
        } else {
          ap.lastSeen = now
          ap.band = (ap.channel >= 36) ? '5G' : '2.4G'
          scanResults.value.push(ap)
        }
      }
      scanResults.value.sort((a, b) => b.rssi - a.rssi)
      scanResultCount.value = scanResults.value.length
      savePersisted()
    } else {
      scanError.value = data.message || 'Scan failed'
    }
  } catch (e) {
    scanError.value = 'Request failed: ' + (e as Error).message
  } finally {
    scanning.value = false
  }
}

const stopDeviceScan = (bssid: string) => {
  if (deviceEventSource.value) {
    deviceEventSource.value.close()
    deviceEventSource.value = null
  }
  if (deviceScanning.value === bssid) {
    deviceScanning.value = null
  }
}

const deauthEventSource = ref<EventSource | null>(null)
const activeDeauthKey = ref<string | null>(null)

const stopDeauth = (key?: string) => {
  if (deauthEventSource.value) {
    deauthEventSource.value.close()
    deauthEventSource.value = null
  }
  const targetKey = key || activeDeauthKey.value
  if (targetKey) {
    deauthing.value[targetKey] = false
  }
  if (!key || activeDeauthKey.value === key) {
    activeDeauthKey.value = null
  }
}

const deauthDevice = async (bssid: string, mac: string, channel: number) => {
  const key = bssid + '-' + mac
  if (deauthing.value[key]) {
    stopDeauth(key)
    return
  }

  stopDeauth()

  deauthing.value[key] = true
  activeDeauthKey.value = key

  try {
    const url = `/api/attack/test-deauth?bssid=${encodeURIComponent(bssid)}&mac=${encodeURIComponent(mac)}&channel=${channel}`
    const es = new EventSource(url)
    deauthEventSource.value = es

    es.addEventListener('result', (event: MessageEvent) => {
      try {
        const data: TestDeauthResponse = JSON.parse(event.data)
        if (data.success) {
          if (data.detected) {
            const delaySec = (data.handshake_delay_ms / 1000).toFixed(1)
            message.success(`Deauth attack on ${mac} Handshake captured (${delaySec}s)`, 8000)
          } else {
            message.warning(`Deauth attack on ${mac} Completed, no handshake captured`, 8000)
          }
        } else {
          message.error(`Deauth attack on ${mac} failed`, 8000)
        }
      } catch (e) {
        message.error(`Failed to parse deauth response: ${(e as Error).message}`, 8000)
      } finally {
        stopDeauth(key)  // 整个sse就一条消息
      }
    })

    es.onerror = () => {
      if (activeDeauthKey.value === key) {
        message.warning(`Deauth attack on ${mac} completed or connection closed`, 5000)
        stopDeauth(key)
      }
    }
  } catch (e) {
    message.error(`Deauth attack on ${mac} failed: ${(e as Error).message}`, 8000)
    stopDeauth(key)
  }
}

const clearDevices = (bssid: string) => {
  if (deviceResults.value[bssid]) {
    deviceResults.value[bssid] = []
  }
  if (deviceErrors.value[bssid]) {
    delete deviceErrors.value[bssid]
  }
  savePersisted()
}

const startDeviceScan = async (bssid: string, channel: number, duration?: number) => {
  if (deviceEventSource.value) {
    stopDeviceScan(deviceScanning.value || '')
  }

  deviceScanning.value = bssid
  deviceErrors.value[bssid] = ''

  // 仅在热点未扫描过（没有历史设备结果）时自动展开
  if (!deviceResults.value[bssid] || deviceResults.value[bssid].length === 0) {
    expandedBssids.value[bssid] = true
  }

  if (!deviceResults.value[bssid]) {
    deviceResults.value[bssid] = []
  }

  // 记录本次扫描前的历史数据作为 Baseline 基准
  // 每当收到SSE更新时将当前会话扫描到的包数与之前的历史基数相加得到总包数，从而实现多次扫描不丢失历史数据且不重复计算
  const baselines: Record<string, { out: number, in: number, handshakes: number }> = {}
  for (const dev of deviceResults.value[bssid]) {
    baselines[dev.mac] = {
      out: dev.packets_out || 0,
      in: dev.packets_in || 0,
      handshakes: dev.handshakes || 0
    }
  }

  try {
    let url = `/api/scan-devices?bssid=${encodeURIComponent(bssid)}&channel=${channel}`
    if (duration !== undefined) {
      url += `&duration=${duration}`
    }
    const es = new EventSource(url)
    deviceEventSource.value = es

    es.onmessage = (event) => {
      try {
        const data: DeviceScanResponse = JSON.parse(event.data)
        if (data.success) {
          const now = new Date().toLocaleString('sv-SE').slice(5)
          for (const dev of data.devices) {
            const existing = deviceResults.value[bssid].find(d => d.mac === dev.mac)
            const base = baselines[dev.mac] || { out: 0, in: 0, handshakes: 0 }
            const newPacketsOut = base.out + (dev.packets_out || 0)
            const newPacketsIn = base.in + (dev.packets_in || 0)
            const newHandshakes = base.handshakes + (dev.handshakes || 0)

            if (existing) {
              // Only update packet counts and lastSeen timestamp when packet counts change
              if (existing.packets_out !== newPacketsOut ||
                  existing.packets_in !== newPacketsIn ||
                  existing.handshakes !== newHandshakes) {
                existing.packets_out = newPacketsOut
                existing.packets_in = newPacketsIn
                existing.handshakes = newHandshakes
                existing.lastSeen = now
              }
              // RSSI: 始终取最新值（只要非零）
              if (dev.rssi) existing.rssi = dev.rssi
            } else {
              // New device insertion
              deviceResults.value[bssid].push({
                mac: dev.mac,
                packets_out: newPacketsOut,
                packets_in: newPacketsIn,
                handshakes: newHandshakes,
                rssi: dev.rssi || 0,
                lastSeen: now
              })
            }
          }

          const ap = scanResults.value.find(a => a.bssid === bssid)
          if (ap) {
            if (data.ap_rssi !== undefined && data.ap_rssi !== 0) {
              ap.rssi = data.ap_rssi
              ap.lastSeen = now
            }
            if (data.ap_beacon_parsed) {
              ap.advanced_info = {
                uptime: data.ap_uptime,
                pmfCapable: data.pmf_capable,
                pmfRequired: data.pmf_required
              }
            }
          }

          savePersisted()
        } else {
          deviceErrors.value[bssid] = data.message || 'Scan failed'
          stopDeviceScan(bssid)
        }
      } catch (err) {
        deviceErrors.value[bssid] = 'Data parsing error'
        stopDeviceScan(bssid)
      }
    }

    es.addEventListener('done', () => {
      stopDeviceScan(bssid)
    })

    es.onerror = () => {
      stopDeviceScan(bssid)
    }
  } catch (e) {
    deviceErrors.value[bssid] = 'Failed to connect: ' + (e as Error).message
    deviceScanning.value = null
  }
}

const formatUptime = (seconds?: number) => {
  if (seconds === undefined) return ''
  const days = Math.floor(seconds / 86400)
  const hours = Math.floor((seconds % 86400) / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)
  
  const parts = []
  if (days > 0) parts.push(`${days}d`)
  if (hours > 0) parts.push(`${hours}h`)
  if (parts.length == 0 || minutes > 0) parts.push(`${minutes}m`)
  return parts.join('')
}

const openMacLookup = (bssid: string) => {
  window.open(`https://maclookup.app/search/result?mac=${bssid}`, 'maclookup', 'width=800,height=600')
}

const rssiColor = (rssi: number): string => {
  if (rssi >= -55) return '#27ae60'
  if (rssi >= -70) return '#f39c12'
  return '#e74c3c'
}

const isVirtualMac = (mac: string): boolean => {
  if (!mac) return false
  const cleanMac = mac.replace(/[^0-9a-fA-F]/g, '')
  if (cleanMac.length < 2) return false
  const firstByte = parseInt(cleanMac.substring(0, 2), 16)
  return !isNaN(firstByte) && (firstByte & 2) !== 0
}

onMounted(() => {
  loadPersisted()
  // 用攻击计划草稿同步"已加入"标记（同标签页内与攻击页共享 sessionStorage）
  for (const t of loadPlan().targets) {
    targetedKeys.value[targetedKey(t.bssid, t.mac)] = true
  }
})

onUnmounted(() => {
  if (deviceEventSource.value) {
    deviceEventSource.value.close()
  }
  if (deauthEventSource.value) {
    deauthEventSource.value.close()
  }
})
</script>

<style scoped lang="scss">
  .btn-col {
    white-space: nowrap;
    button:last-of-type {
      border-top-right-radius: 0;
      border-bottom-right-radius: 0;
    }
    :deep(details summary[role="button"]) {
      border-top-left-radius: 0;
      border-bottom-left-radius: 0;
      border-left: 0;
      padding-left: 0;
    }
  }
  .adv-ap-inf {
    font-size: 0.8em;
    color: var(--primary);
    display: flex;
    gap: 0.8rem;
    align-items: center;
    margin: 0 1rem;
  }
  .dev-table-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    cursor: pointer;
    user-select: none;
    padding:0.5rem 1rem;
  }
  .dev-table-table {
    padding: 0 1rem;
  }
  .smaller-td {
    font-size:0.85em;
  }
  .dbm-unit {
    font-size: 0.6rem;
  }
  .disabled-link {
    color: var(--pico-muted-color) !important;
    cursor: not-allowed !important;
    pointer-events: none !important;
  }
</style>