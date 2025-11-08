// script.js — 4 actuadores + 4 sliders PWM (debounced)
window.addEventListener('DOMContentLoaded', () => {
  console.log('script.js cargado');

  let tempChart = null;
  let humChart = null;

  // Historiales y estados para 4 actuadores
  let actuatorHistories = {1:[],2:[],3:[],4:[]};
  let actuatorStates = {1:false,2:false,3:false,4:false};

  // PWM values local cache (0-100)
  let pwmValues = {1:0,2:0,3:0,4:0};
  let pwmTimeouts = {1:null,2:null,3:null,4:null};

  function setTextAny(ids, text) {
    ids.forEach(id => {
      const el = document.getElementById(id);
      if (el) el.textContent = text;
    });
  }

  async function fetchData() {
    try {
      const res = await fetch('/api/data');
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const data = await res.json();

      const temp = data?.temp ?? null;
      const hum  = data?.hum  ?? null;

      setTextAny(['temp'], temp !== null ? Number(temp).toFixed(1) : '--');
      setTextAny(['hum'], hum  !== null ? Number(hum).toFixed(1)  : '--');

      // Si el servidor devuelve estados de actuadores
      if (Array.isArray(data.actuators)) {
        data.actuators.forEach((st,i)=>updateActuatorUI(i+1, st));
      }
    } catch (err) {
      console.error('fetchData error:', err);
      setTextAny(['temp'], '--');
      setTextAny(['hum'], '--');
    }
  }

  // ========== Actuator UI ==========
  function updateActuatorUI(id, state) {
    const toggle = document.getElementById(`actuatorToggle${id}`);
    const status = document.getElementById(`actuatorStatus${id}`);
    if (toggle) toggle.checked = state;
    if (status) status.textContent = state ? 'ENCENDIDO' : 'APAGADO';
    actuatorStates[id] = state;
  }

  async function sendActuatorCommand(id, state) {
    try {
      const res = await fetch(`/actuator?id=${id}&state=${state?"on":"off"}`);
      if (!res.ok) throw new Error();
      const result = await res.json();
      updateActuatorUI(id, result.actuator);

      const name = document.getElementById(`actuatorName${id}`).value || `Actuador ${id}`;
      const action = state ? "ENCENDIDO" : "APAGADO";

      actuatorHistories[id].push(`${new Date().toLocaleString()} - ${name}: ${action}`);
      Swal.fire({title:"¡Éxito!", text:`${name} ${action.toLowerCase()}`, icon:"success", timer:1200, showConfirmButton:false, background:"#1e1e1e", color:"#fff"});
    } catch (error) {
      console.error("Error controlando actuador:", error);
      updateActuatorUI(id, actuatorStates[id]);
      Swal.fire({title:"Error", text:"No se pudo controlar el actuador", icon:"error", background:"#1e1e1e", color:"#fff"});
    }
  }

  function setupActuatorToggle(id) {
    const toggle = document.getElementById(`actuatorToggle${id}`);
    if (!toggle) return;

    toggle.addEventListener('change', function() {
      const desiredState = this.checked;
      if (desiredState === actuatorStates[id]) {
        this.checked = actuatorStates[id];
        return;
      }

      const action = desiredState ? "encender" : "apagar";
      Swal.fire({
        title: "¿Estás seguro?",
        text: `¿Quieres ${action} este actuador?`,
        icon: "question",
        showCancelButton: true,
        confirmButtonText: `Sí, ${action}`,
        cancelButtonText: "Cancelar",
        background: "#1e1e1e",
        color: "#fff"
      }).then((result) => {
        if (result.isConfirmed) {
          sendActuatorCommand(id, desiredState);
        } else {
          this.checked = actuatorStates[id];
        }
      });
    });
  }

  [1,2,3,4].forEach(setupActuatorToggle);

  // ========== PWM / Sliders ==========
  function updateSliderUI(id, value) {
    const slider = document.getElementById(`pwmSlider${id}`);
    const label = document.getElementById(`pwmValue${id}`);
    if (slider) slider.value = value;
    if (label) label.textContent = value;
    pwmValues[id] = value;
  }

  async function sendPWM(id, value) {
    try {
      const res = await fetch(`/pwm?id=${id}&value=${value}`);
      if (!res.ok) throw new Error();
      // optional: const result = await res.json();
    } catch (e) {
      console.error("Error enviando PWM", e);
    }
  }

  function setupSlider(id) {
    const slider = document.getElementById(`pwmSlider${id}`);
    const valueLabel = document.getElementById(`pwmValue${id}`);
    if (!slider || !valueLabel) return;

    valueLabel.textContent = "0";
    slider.value = 0;
    pwmValues[id] = 0;

    slider.addEventListener('input', function() {
      const v = parseInt(this.value);
      valueLabel.textContent = v;
      if (pwmTimeouts[id]) clearTimeout(pwmTimeouts[id]);
      pwmTimeouts[id] = setTimeout(() => {
        sendPWM(id, v);
        pwmTimeouts[id] = null;
      }, 200);
    });

    slider.addEventListener('change', function() {
      const v = parseInt(this.value);
      valueLabel.textContent = v;
      if (pwmTimeouts[id]) clearTimeout(pwmTimeouts[id]);
      sendPWM(id, v);
      pwmTimeouts[id] = null;
    });
  }

  [1,2,3,4].forEach(setupSlider);

  // ========== Charts / History ==========
  function updateCharts(labels = [], temps = [], hums = []) {
    const tempCtx = document.getElementById('tempChart');
    if (tempCtx) {
      if (!tempChart) {
        tempChart = new Chart(tempCtx.getContext('2d'), {
          type: 'line',
          data: { labels: labels, datasets: [{ label: 'Temperatura (°C)', data: temps, borderColor: 'red', fill: false }] },
          options: { responsive: true }
        });
      } else {
        tempChart.data.labels = labels;
        tempChart.data.datasets[0].data = temps;
        tempChart.update();
      }
    }

    const humCtx = document.getElementById('humChart');
    if (humCtx) {
      if (!humChart) {
        humChart = new Chart(humCtx.getContext('2d'), {
          type: 'line',
          data: { labels: labels, datasets: [{ label: 'Humedad (%)', data: hums, borderColor: 'blue', fill: false }] },
          options: { responsive: true }
        });
      } else {
        humChart.data.labels = labels;
        humChart.data.datasets[0].data = hums;
        humChart.update();
      }
    }
  }

  async function loadHistory() {
    const dateInput = document.getElementById('historyDate');
    const date = dateInput ? dateInput.value : null;
    if (!date) return;

    try {
      const res = await fetch(`/api/history?date=${date}`);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = await res.json();

      if (!Array.isArray(data) || data.length === 0) {
        updateCharts([], [], []);
        Swal.fire({title:"Sin datos", text:"No hay registros para la fecha seleccionada.", icon:"info"});
        return;
      }

      const labels = data.map(e => e.time || "");
      const temps  = data.map(e => Number(e.temp) || null);
      const hums   = data.map(e => Number(e.hum)  || null);

      updateCharts(labels, temps, hums);
    } catch (err) {
      console.error('loadHistory error:', err);
      Swal.fire({title:"Error", text:"No se pudieron cargar los datos.", icon:"error"});
    }
  }

  window.showCharts = () => { document.getElementById('chartsModal').style.display = 'flex'; loadHistory(); };
  window.closeCharts = () => { document.getElementById('chartsModal').style.display = 'none'; };

  window.showActuatorHistory = (id) => {
    const list = document.getElementById('actuatorHistoryList');
    list.innerHTML = "";
    if (actuatorHistories[id].length === 0) {
      list.innerHTML = "<li>Sin registros aún</li>";
    } else {
      actuatorHistories[id].forEach(it => {
        const li = document.createElement('li');
        li.textContent = it;
        list.appendChild(li);
      });
    }
    document.getElementById('historyPopup').style.display = 'flex';
  };
  window.closeHistoryPopup = () => document.getElementById('historyPopup').style.display = 'none';
  window.loadHistory = loadHistory;

  // Botón de tema
  const themeBtn = document.getElementById('themeToggle');
  if (themeBtn) themeBtn.addEventListener('click', () => document.body.classList.toggle('light-mode'));

  fetchData();
  setInterval(fetchData, 3000);
});
