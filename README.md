# MPMC / Lock-free MPMC Circular Buffer

Bu repo, çok üreticili / çok tüketicili (MPMC) lock-free halka buffer örneğini içerir. Proje kodu `MPMC/` klasörü altındadır.

## Özellikler
- Lock-free, bounded MPMC ring buffer (sequence counter deseni, 2^n kapasite)
- CPU tarafında sabit boyutlu char chunk; GPU simülasyonu için short chunk
- Ek metadata: `rfSignal (std::pair<int,double>)` ve `size (std::size_t)`
- LOG_DEBUG tanımlı derlemelerde thread-safe log ve küçük gecikme simülasyonu
- Docker tabanlı geliştirme ortamı (Ubuntu 24.04, build-essential, gdb, cmake, ninja, clang-format)

## Hızlı Başlangıç (Docker + Compose)
```bash
cd /home/user/works/MPMC
docker compose up -d --build       # mpmc-dev imajını build edip container'ı başlatır
docker compose exec mpmc-dev /bin/bash
cd /workspace/MPMC
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/app
```

## Yerel Derleme (Docker olmadan)
```bash
cd /home/user/works/MPMC/MPMC
g++ -std=c++20 -O2 -pthread main.cpp -o app
./app
```

## LOG_DEBUG
`CMakeLists.txt` içinde `LOG_DEBUG` tanımlıdır. Eğer log ve gecikme istemezseniz `target_compile_definitions` satırını yorumlayın veya `-DLOG_DEBUG` vermeyin.

## Kod Yapısı (MPMC/main.cpp)
- `CircularBuffer`: Lock-free MPMC ring buffer; sequence counter ile boş/dolu durumu.
- Buffer katmanları:
  - `data_cpu_`: char chunk’lar
  - `data_gpu_`: short chunk’lar (simülasyon)
  - `meta_rf_signal_`: `std::pair<int,double>`
  - `meta_size_`: yazılan byte sayısı
- Producer akışı: claim → CPU chunk'a string yaz → metadata set → GPU buffer'a anlamlı short'lar (value, producer id) yaz → commit.
- Consumer akışı: claim → log (LOG_DEBUG) → release.

## Docker Notları
- Compose dosyasındaki `version` alanı kaldırıldı (uyarı vermesin).
- Docker daemon erişimi için `docker` grubunda olun; gerekirse:
  ```bash
  sudo groupadd docker        # yoksa
  sudo usermod -aG docker $USER
  newgrp docker
  docker ps
  ```

## Temizlik
`build/` klasörleri git'te ignore edilir. Temizlemek için:
```bash
rm -rf build
rm -rf MPMC/build
```

## Lisans
Bu repo için ayrı bir lisans dosyası eklenmedi; kurum içi/demo amaçlıdır.

