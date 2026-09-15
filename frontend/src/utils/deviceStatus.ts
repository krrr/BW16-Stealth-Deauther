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

export const fetchDeviceStatus = async (): Promise<DeviceStatus | null> => {
  try {
    const res = await fetch('/api/status')
    if (res.ok) {
      const data: DeviceStatus = await res.json()
      status.value = data
      return data
    }
  } catch (err) {
    console.error('Failed to fetch device status:', err)
  }
  return null
}

export const useDeviceStatus = () => {
  return { status, fetchDeviceStatus, }
}
