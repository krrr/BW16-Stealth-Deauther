<template>
  <dialog :open="open" @click.self="onBackdropClick">
    <article class="rssi-modal-article">
      <header>
        <p class="modal-title">
          <strong>Signal Graph: {{ target?.ssid || '(Hidden SSID)' }}</strong>
        </p>
        <div class="flex1"></div>
        <button aria-label="Close" rel="prev" @click="closeModal"></button>
      </header>

      <!-- Canvas Chart Container -->
      <div class="canvas-container" ref="containerRef">
        <canvas ref="canvasRef"></canvas>
      </div>

      <!-- Signal Quality Legend & Status Bar -->
      <div class="legend-bar">
        <div class="legend-items">
          <span class="legend-tag tag-green"><span class="dot"></span> Excellent (&ge; -55)</span>
          <span class="legend-tag tag-yellow"><span class="dot"></span> Good (-56 ~ -70)</span>
          <span class="legend-tag tag-red"><span class="dot"></span> Weak (&lt; -70)</span>
        </div>
      </div>
    </article>
  </dialog>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted, nextTick } from 'vue'

export interface RssiPoint {
  time: string
  timestamp: number
  rssi: number
}

export interface RssiTarget {
  bssid: string
  ssid?: string
  rssi: number
  channel?: number
  band?: string
  lastSeen?: string
}

const props = defineProps<{
  open: boolean
  target: RssiTarget | null
  history: RssiPoint[]
}>()

const emit = defineEmits<{
  (e: 'close'): void
}>()

const containerRef = ref<HTMLDivElement | null>(null)
const canvasRef = ref<HTMLCanvasElement | null>(null)

const closeModal = () => {
  emit('close')
}

const onBackdropClick = () => {
  closeModal()
}

const handleKeyDown = (e: KeyboardEvent) => {
  if (e.key === 'Escape' && props.open) {
    closeModal()
  }
}

const rssiColor = (rssi: number): string => {
  if (rssi >= -55) return '#27ae60'
  if (rssi >= -70) return '#f39c12'
  return '#e74c3c'
}

const drawChart = () => {
  const canvas = canvasRef.value
  const container = containerRef.value
  if (!canvas || !container || !props.open) return

  const dpr = window.devicePixelRatio || 1
  const width = container.clientWidth || 520
  const height = 420

  canvas.width = Math.floor(width * dpr)
  canvas.height = Math.floor(height * dpr)
  canvas.style.width = width + 'px'
  canvas.style.height = height + 'px'

  const ctx = canvas.getContext('2d')
  if (!ctx) return

  ctx.resetTransform()
  ctx.scale(dpr, dpr)
  ctx.clearRect(0, 0, width, height)

  const isDark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches

  const paddingLeft = 54
  const paddingRight = 24
  const paddingTop = 26
  const paddingBottom = 26
  const plotWidth = width - paddingLeft - paddingRight
  const plotHeight = height - paddingTop - paddingBottom

  // Dynamic Y-axis scale based on data
  let minDbm = -95
  let maxDbm = -20
  if (props.history.length > 0) {
    const dataMin = Math.min(...props.history.map(p => p.rssi))
    const dataMax = Math.max(...props.history.map(p => p.rssi))
    if (dataMin < minDbm) minDbm = Math.floor(dataMin / 10) * 10 - 5
    if (dataMax > maxDbm) maxDbm = Math.ceil(dataMax / 10) * 10 + 5
  }

  const getY = (val: number) => {
    const clamped = Math.max(minDbm, Math.min(maxDbm, val))
    return paddingTop + plotHeight * (1 - (clamped - minDbm) / (maxDbm - minDbm))
  }

  const getX = (index: number, total: number) => {
    if (total <= 1) return paddingLeft + plotWidth / 2
    return paddingLeft + (index / (total - 1)) * plotWidth
  }

  // Draw signal zones (subtle colored background bands)
  const drawZone = (upper: number, lower: number, fill: string) => {
    const top = getY(Math.min(maxDbm, upper))
    const bottom = getY(Math.max(minDbm, lower))
    if (bottom > top) {
      ctx.fillStyle = fill
      ctx.fillRect(paddingLeft, top, plotWidth, bottom - top)
    }
  }

  drawZone(maxDbm, -55, isDark ? 'rgba(39, 174, 96, 0.18)' : 'rgba(39, 174, 96, 0.13)')
  drawZone(-55, -70, isDark ? 'rgba(243, 156, 18, 0.18)' : 'rgba(243, 156, 18, 0.13)')
  drawZone(-70, minDbm, isDark ? 'rgba(231, 76, 60, 0.18)' : 'rgba(231, 76, 60, 0.13)')

  // Draw Grid Lines & Y-axis Labels
  const gridLevels = [-30, -45, -60, -75, -90]
  ctx.font = '11px -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace'
  ctx.textAlign = 'right'
  ctx.textBaseline = 'middle'

  for (const level of gridLevels) {
    if (level <= minDbm || level >= maxDbm) continue
    const y = getY(level)

    // Dashed grid line
    ctx.beginPath()
    ctx.strokeStyle = isDark ? 'rgba(255, 255, 255, 0.1)' : 'rgba(0, 0, 0, 0.08)'
    ctx.lineWidth = 1
    ctx.setLineDash([4, 4])
    ctx.moveTo(paddingLeft, y)
    ctx.lineTo(paddingLeft + plotWidth, y)
    ctx.stroke()
    ctx.setLineDash([])

    // Label
    ctx.fillStyle = isDark ? 'rgba(255, 255, 255, 0.5)' : 'rgba(0, 0, 0, 0.5)'
    ctx.fillText(`${level}`, paddingLeft - 8, y)
  }

  // Outer border around plot area
  ctx.strokeStyle = isDark ? 'rgba(255, 255, 255, 0.15)' : 'rgba(0, 0, 0, 0.12)'
  ctx.lineWidth = 1
  ctx.strokeRect(paddingLeft, paddingTop, plotWidth, plotHeight)

  const data = props.history

  if (data.length === 0) {
    // Empty state
    ctx.fillStyle = isDark ? 'rgba(255, 255, 255, 0.4)' : 'rgba(0, 0, 0, 0.4)'
    ctx.textAlign = 'center'
    ctx.fillText('No signal samples recorded yet', paddingLeft + plotWidth / 2, paddingTop + plotHeight / 2)
    return
  }

  if (data.length === 1) {
    // Single point: draw horizontal reference line and point
    const p = data[0]
    const y = getY(p.rssi)
    const x = paddingLeft + plotWidth / 2

    ctx.beginPath()
    ctx.strokeStyle = isDark ? 'rgba(56, 189, 248, 0.4)' : 'rgba(2, 132, 199, 0.4)'
    ctx.lineWidth = 1.5
    ctx.setLineDash([5, 5])
    ctx.moveTo(paddingLeft, y)
    ctx.lineTo(paddingLeft + plotWidth, y)
    ctx.stroke()
    ctx.setLineDash([])

    // Point
    ctx.beginPath()
    ctx.arc(x, y, 6, 0, Math.PI * 2)
    ctx.fillStyle = rssiColor(p.rssi)
    ctx.fill()
    ctx.lineWidth = 2
    ctx.strokeStyle = isDark ? '#1a1f2c' : '#ffffff'
    ctx.stroke()

    // Value badge
    drawValueBadge(ctx, x, y, `${p.rssi}`, rssiColor(p.rssi), isDark)

    // Bottom time label
    ctx.fillStyle = isDark ? 'rgba(255, 255, 255, 0.5)' : 'rgba(0, 0, 0, 0.5)'
    ctx.textAlign = 'center'
    ctx.fillText(p.time, x, paddingTop + plotHeight + 16)
    return
  }

  // Gradient area fill
  const gradient = ctx.createLinearGradient(0, paddingTop, 0, paddingTop + plotHeight)
  gradient.addColorStop(0, isDark ? 'rgba(56, 189, 248, 0.45)' : 'rgba(2, 132, 199, 0.35)')
  gradient.addColorStop(1, isDark ? 'rgba(56, 189, 248, 0.0)' : 'rgba(2, 132, 199, 0.0)')

  ctx.beginPath()
  ctx.moveTo(getX(0, data.length), getY(data[0].rssi))
  for (let i = 1; i < data.length; i++) {
    ctx.lineTo(getX(i, data.length), getY(data[i].rssi))
  }
  ctx.lineTo(getX(data.length - 1, data.length), paddingTop + plotHeight)
  ctx.lineTo(getX(0, data.length), paddingTop + plotHeight)
  ctx.closePath()
  ctx.fillStyle = gradient
  ctx.fill()

  // Main Trend Line
  ctx.beginPath()
  ctx.strokeStyle = isDark ? '#38bdf8' : '#0284c7'
  ctx.lineWidth = 2.5
  ctx.lineJoin = 'round'
  ctx.lineCap = 'round'

  for (let i = 0; i < data.length; i++) {
    const x = getX(i, data.length)
    const y = getY(data[i].rssi)
    if (i === 0) {
      ctx.moveTo(x, y)
    } else {
      ctx.lineTo(x, y)
    }
  }
  ctx.stroke()

  // Latest point marker & value badge
  const lastIdx = data.length - 1
  const lastP = data[lastIdx]
  const lastX = getX(lastIdx, data.length)
  const lastY = getY(lastP.rssi)

  // Pulsing outer halo for latest point
  ctx.beginPath()
  ctx.arc(lastX, lastY, 9, 0, Math.PI * 2)
  ctx.fillStyle = isDark ? 'rgba(56, 189, 248, 0.25)' : 'rgba(2, 132, 199, 0.25)'
  ctx.fill()

  // Inner point
  ctx.beginPath()
  ctx.arc(lastX, lastY, 5, 0, Math.PI * 2)
  ctx.fillStyle = rssiColor(lastP.rssi)
  ctx.fill()
  ctx.lineWidth = 2
  ctx.strokeStyle = isDark ? '#1a1f2c' : '#ffffff'
  ctx.stroke()

  // Current value badge
  drawValueBadge(ctx, lastX, lastY, `${lastP.rssi}`, rssiColor(lastP.rssi), isDark)

  // Time labels on X-axis (First, Middle, Last)
  ctx.fillStyle = isDark ? 'rgba(255, 255, 255, 0.5)' : 'rgba(0, 0, 0, 0.5)'
  ctx.font = '10px -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace'

  // First time
  ctx.textAlign = 'left'
  ctx.fillText(data[0].time, getX(0, data.length), paddingTop + plotHeight + 16)

  // Middle time (if >= 4 points)
  if (data.length >= 4) {
    const midIdx = Math.floor(data.length / 2)
    ctx.textAlign = 'center'
    ctx.fillText(data[midIdx].time, getX(midIdx, data.length), paddingTop + plotHeight + 16)
  }

  // Last time
  ctx.textAlign = 'right'
  ctx.fillText(data[data.length - 1].time, getX(data.length - 1, data.length), paddingTop + plotHeight + 16)
}

const drawValueBadge = (
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  text: string,
  color: string,
  isDark: boolean
) => {
  ctx.font = 'bold 11px -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace'
  const textWidth = ctx.measureText(text).width
  const badgeWidth = textWidth + 12
  const badgeHeight = 20
  let badgeX = x - badgeWidth / 2
  let badgeY = y - 26

  // Keep badge inside chart bounds
  if (badgeX < 60) badgeX = 60
  if (badgeY < 6) badgeY = y + 10

  // Badge background box
  ctx.fillStyle = color
  ctx.beginPath()
  ctx.roundRect(badgeX, badgeY, badgeWidth, badgeHeight, 4)
  ctx.fill()

  // Badge text
  ctx.fillStyle = '#ffffff'
  ctx.textAlign = 'center'
  ctx.textBaseline = 'middle'
  ctx.fillText(text, badgeX + badgeWidth / 2, badgeY + badgeHeight / 2)
}

let resizeObserver: ResizeObserver | null = null

watch(
  () => [props.open, props.history, props.target],
  () => {
    if (props.open) {
      nextTick(() => {
        drawChart()
      })
    }
  },
  { deep: true }
)

onMounted(() => {
  window.addEventListener('keydown', handleKeyDown)
  if (containerRef.value) {
    resizeObserver = new ResizeObserver(() => {
      if (props.open) drawChart()
    })
    resizeObserver.observe(containerRef.value)
  }
})

onUnmounted(() => {
  window.removeEventListener('keydown', handleKeyDown)
  if (resizeObserver) {
    resizeObserver.disconnect()
    resizeObserver = null
  }
})
</script>

<style scoped lang="scss">
.rssi-modal-article {
  max-width: 620px;
  width: 95vw;
  margin: 0 auto;
}

.stats-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 0.5rem;
  margin-bottom: 1rem;

  .stat-card {
    background: var(--pico-card-background-color, rgba(0, 0, 0, 0.03));
    border: 1px solid var(--pico-muted-border-color);
    border-radius: var(--pico-border-radius);
    padding: 0.4rem 0.6rem;
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;

    .stat-label {
      font-size: 0.75rem;
      color: var(--pico-muted-color);
      margin-bottom: 0.15rem;
      text-transform: uppercase;
      letter-spacing: 0.03em;
    }

    .stat-val {
      font-size: 1.1rem;
      font-weight: 700;
      line-height: 1.2;

      .dbm-unit {
        font-size: 0.65rem;
        font-weight: normal;
        color: var(--pico-muted-color);
      }
    }
  }
}

@media (max-width: 480px) {
  .stats-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}

.canvas-container {
  width: 100%;
  height: 420px;
  position: relative;
  margin-bottom: 0.75rem;
  background: var(--pico-card-background-color, rgba(0, 0, 0, 0.02));
  border-radius: var(--pico-border-radius);
  overflow: hidden;

  canvas {
    display: block;
    width: 100%;
    height: 100%;
  }
}

.legend-bar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
  gap: 0.5rem;
  font-size: 0.7rem;
  padding-top: 0.35rem;

  .legend-items {
    display: flex;
    gap: 0.6rem;
    flex-wrap: wrap;

    .legend-tag {
      display: inline-flex;
      align-items: center;
      gap: 0.25rem;
      color: var(--pico-muted-color);

      .dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        display: inline-block;
      }
      &.tag-green .dot { background-color: #27ae60; }
      &.tag-yellow .dot { background-color: #f39c12; }
      &.tag-red .dot { background-color: #e74c3c; }
    }
  }
}

</style>
