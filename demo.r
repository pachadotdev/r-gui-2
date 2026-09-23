x <- 1
y <- 2

x + y

library(tinyplot)

aq = transform(
  airquality,
  Month = factor(Month, labels = month.abb[unique(Month)]),
  Hot = Temp > median(Temp)
)

tinyplot(Temp ~ Day | Month, data = aq)
