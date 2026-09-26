const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('telemetry', {
  onUpdate: (callback) => {
    ipcRenderer.on('data', (event, data) => callback(data));
  },
  onStatus: (callback) => {
    ipcRenderer.on('status', (event, status) => callback(status));
  }
});
