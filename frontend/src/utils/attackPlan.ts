// 攻击计划草稿：仅存 sessionStorage（按标签页隔离），
// 扫描页加入目标 / 攻击页删除目标都只改草稿，
// 点击"开始/停止"时由 AttackView 组装完整计划一次性提交设备闪存。

export interface AttackTarget {
  mac: string
  bssid: string
  ssid: string
  channel: number
  rssi: number | null
  lastSeen?: string
  addedAt?: string
}

export interface AttackConfig {
  type: string        // 'deauth'
  intervalSec: number // 1..3600
  psEnable: boolean
}

export interface AttackPlan {
  targets: AttackTarget[]
  config: AttackConfig
}

const STORAGE_KEY = 'attack_plan_draft'

export const defaultConfig = (): AttackConfig => ({
  type: 'deauth',
  intervalSec: 5,
  psEnable: false,
})

export const defaultPlan = (): AttackPlan => ({
  targets: [],
  config: defaultConfig(),
})

export const hasPlan = (): boolean => {
  try {
    return sessionStorage.getItem(STORAGE_KEY) !== null
  } catch {
    return false
  }
}

export const loadPlan = (): AttackPlan => {
  try {
    const raw = sessionStorage.getItem(STORAGE_KEY)
    if (!raw) return defaultPlan()
    const p = JSON.parse(raw)
    return {
      targets: Array.isArray(p.targets) ? p.targets : [],
      config: { ...defaultConfig(), ...(p.config || {}) },
    }
  } catch {
    return defaultPlan()
  }
}

export const savePlan = (plan: AttackPlan) => {
  try {
    sessionStorage.setItem(STORAGE_KEY, JSON.stringify(plan))
  } catch { /* ignore quota errors */ }
}

export const addTarget = (t: AttackTarget): AttackPlan => {
  const plan = loadPlan()
  if (plan.targets.some(x => x.mac === t.mac && x.bssid === t.bssid)) return plan
  const added: AttackTarget = {
    ...t,
    lastSeen: t.lastSeen || '',
    addedAt: new Date().toLocaleString('sv-SE'),
  }
  plan.targets.push(added)
  savePlan(plan)
  return plan
}

export const removeTarget = (mac: string, bssid: string): AttackPlan => {
  const plan = loadPlan()
  plan.targets = plan.targets.filter(x => !(x.mac === mac && x.bssid === bssid))
  savePlan(plan)
  return plan
}

export const hasTarget = (mac: string, bssid: string): boolean => {
  return loadPlan().targets.some(x => x.mac === mac && x.bssid === bssid)
}

export const clearPlan = () => {
  try {
    sessionStorage.removeItem(STORAGE_KEY)
  } catch { /* ignore */ }
}
