// emdawnwebgpu passes setBindGroup's dynamic offsets as the whole HEAPU32 plus
// an offset and length. Firefox rejects any view over 2 GB, which a wasm64 heap
// holding guest memory always is, so pass just the offsets instead, as a plain
// array (node's Dawn takes no typed array without the offset and length).
if (typeof GPURenderPassEncoder !== 'undefined') {
  [GPURenderPassEncoder, GPUComputePassEncoder, GPURenderBundleEncoder].forEach((type) => {
    const setBindGroup = type.prototype.setBindGroup;
    type.prototype.setBindGroup = function (index, group, offsets, start, length) {
      if (ArrayBuffer.isView(offsets) && start !== undefined) {
        return setBindGroup.call(this, index, group, Array.from(offsets.subarray(start, start + length)));
      }
      return setBindGroup.apply(this, arguments);
    };
  });
}
