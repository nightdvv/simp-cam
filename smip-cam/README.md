# SimpCam

Windows için hafif, reklamsız ve açık kaynaklı bir kamera uygulaması. İlk sürüm
Media Foundation ve Win32 kullanarak kamera seçimi ve canlı önizleme sunar.

## Gereksinimler

- Windows 10 veya üzeri
- Visual Studio 2022 (C++ masaüstü geliştirme bileşeni)
- CMake 3.20 veya üzeri

## Derleme

Visual Studio'da bu klasörü açıp **SimpCam** hedefini derleyin. Alternatif
olarak Visual Studio Geliştirici Komut İstemi'nde:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Program kayıt yapmaz; canlı önizleme için gereken son kareyi bellekte tutar.
Pencereyi kapatınca kamera yakalama iş parçacığı durdurulur; sistem tepsisine
küçültme yapılmaz.

Lisans: MIT. Ayrıntılar için [LICENSE](LICENSE) dosyasına bakın.
