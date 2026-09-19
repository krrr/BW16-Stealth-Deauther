import { ref } from 'vue'

export interface BatteryInfo {
  connected: boolean
  voltage: number
  level: number
  percent: number
}

export interface DeviceStatus {
  uptime: number
  ap_channel: number
  compile_date: string
  compile_time: string
  rtc_time?: number
  free_heap?: number
  ap_saver_state?: string
  attack_running?: boolean
  battery?: BatteryInfo
}

const status = ref<DeviceStatus>({
  uptime: -1,
  ap_channel: 1,
  compile_date: '',
  compile_time: '',
  battery: {
    connected: false,
    voltage: 0,
    level: 0,
    percent: 0,
  }
})

const statusLoading = ref(false)
const statusError = ref<string | null>(null)

export const fetchDeviceStatus = async (): Promise<DeviceStatus | null> => {
  statusLoading.value = true
  statusError.value = null
  try {
    const res = await fetch('/api/status')
    if (!res.ok) {
      throw new Error(`HTTP error ${res.status}`)
    }
    const data: DeviceStatus = await res.json()
    status.value = data
    return data
  } catch (err: any) {
    console.error('Failed to fetch device status:', err)
    statusError.value = err?.message || 'Failed to fetch status'
    return null
  } finally {
    statusLoading.value = false
  }
}

export const useDeviceStatus = () => {
  return { status, statusLoading, statusError, fetchDeviceStatus }
}
