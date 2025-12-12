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
  - `data_cpu_`: char chunk'lar
  - `data_gpu_`: short chunk'lar (simülasyon)
  - `meta_rf_signal_`: `std::pair<int,double>`
  - `meta_size_`: yazılan byte sayısı
- Producer akışı: claim → CPU chunk'a string yaz → metadata set → GPU buffer'a anlamlı short'lar (value, producer id) yaz → commit.
- Consumer akışı: claim → log (LOG_DEBUG) → release.

## RAII Wrapper (Exception Safety - ÖNERİLEN)
Producer/Consumer fail olsa bile (exception, early return) slot'ların kaybolmaması için RAII wrapper kullanın:

**Producer:**
```cpp
if (auto ticket = buffer.claim_producer_raii()) {
    // ticket->cpu_ptr, ticket->gpu_ptr, ticket->rf, ticket->size_ptr kullan
    // ... veri yaz ...
    ticket->commit();  // Manuel commit (isteğe bağlı, destructor zaten yapar)
} // Destructor otomatik commit yapar
```

**Consumer:**
```cpp
if (auto ticket = buffer.claim_consumer_raii()) {
    // ticket->cpu_ptr, ticket->gpu_ptr, ticket->rf, ticket->size_ptr kullan
    // ... veri oku ...
    ticket->release();  // Manuel release (isteğe bağlı, destructor zaten yapar)
} // Destructor otomatik release yapar
```

**Not:** Eski API (`claim_producer()`, `claim_consumer()`) hala çalışıyor ama exception safety yok. RAII wrapper kullanmanız önerilir.

## Docker Notları
- Compose dosyasındaki `version` alanı kaldırıldı (uyarı vermesin).
- Docker daemon erişimi için `docker` grubunda olun; gerekirse:
  ```bash
  sudo groupadd docker        # yoksa
  sudo usermod -aG docker $USER
  newgrp docker
  docker ps
  ```

## Test Suite

Test dosyası (`test.cpp`) projede mevcuttur. Testleri çalıştırmak için:

**Container içinde:**
```bash
cd /workspace/MPMC
g++ -std=c++20 -O2 -pthread test.cpp -o test_app
./test_app
```

**Local'de:**
```bash
cd /home/user/works/MPMC/MPMC
g++ -std=c++20 -O2 -pthread test.cpp -o test_app
./test_app
```

Test dosyası (`test.cpp`) şu testleri içerir:
1. **test_basic_producer_consumer**: Temel producer/consumer işlevselliği
2. **test_non_blocking**: Veri yoksa nullopt dönmesi
3. **test_exception_safety**: Producer fail olsa bile slot kaybolmaması (tail_ commit'te artırılıyor)
4. **test_multiple_producer_consumer**: Çoklu thread producer/consumer
5. **test_shutdown**: Shutdown sonrası nullopt dönmesi
6. **test_capacity_limit**: Buffer dolu olduğunda nullopt dönmesi
7. **test_thread_safety**: Thread safety (race condition testi)
8. **test_commit_increments_tail**: Commit sonrası tail artışı

Test sonuçları terminalde görüntülenir ve özet rapor sunulur.

## Permission Denied Sorunu (WSL)
Docker container içinde root olarak oluşturulan dosyalar host'ta da root sahipliğinde kalır. Bu yüzden `user` kullanıcısı yazamaz.

**Çözüm:**
```bash
# Klasör sahipliğini düzelt
sudo chown -R user:user /home/user/works/MPMC/MPMC

# Veya tüm MPMC klasörü için
sudo chown -R user:user /home/user/works/MPMC
```

**Önleme:** Container içinde dosya oluştururken `--user dev` kullanın veya host'ta dosya oluşturun.

## Temizlik
`build/` klasörleri git'te ignore edilir. Temizlemek için:
```bash
rm -rf build
rm -rf MPMC/build
```

## Lisans
Bu repo için ayrı bir lisans dosyası eklenmedi; kurum içi/demo amaçlıdır.

